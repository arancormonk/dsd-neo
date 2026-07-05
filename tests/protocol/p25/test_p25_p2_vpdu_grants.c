// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25p2 MAC VPDU grant tests: MFID 0x90 regroup grants (A3/A4) and UU grants (0x44).
 * Asserts trunking tune side-effects via test shim capture.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <stdbool.h>
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

#include "p25_test_shim.h"

struct RtlSdrContext;

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
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* tag, const char* text, const char* needle) {
    if (text == NULL || needle == NULL || strstr(text, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: '%s' did not contain '%s'\n", tag, text ? text : "(null)", needle ? needle : "(null)");
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Common IDEN: iden=1, type=1 (FDMA), spac=12.5k, base=851.000 MHz
    // Note: base/spacing units match process_channel_to_freq expectations:
    // base is in 5 Hz units; spacing is in 125 Hz units.
    const int iden = 1, type = 1, tdma = 0, spac = 100; // 100*125 = 12.5 kHz
    const long base = 170200000;                        // 170200000*5 = 851,000,000 Hz
    const long cc = 851000000;                          // non-zero CC freq enables tuning

    // Case A: MFID 0x90, opcode A3 (Group Regroup Channel Grant - Implicit)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0xA3;
        mac[2] = 0x90;
        mac[5] = 0x10;
        mac[6] = 0x0A; // channel 0x100A -> 851.125 MHz
        mac[7] = 0x45;
        mac[8] = 0x67; // group id (arbitrary)
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {iden, type, tdma, base, spac};
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("A3 tuned", tuned == 1);
        rc |= expect_eq_long("A3 vc", vc, 851125000);
    }

    // Case B: UU Voice Service Channel Grant (opcode 0x44)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x44;
        mac[2] = 0x00; // std MFID
        mac[2] = 0x10;
        mac[3] = 0x0A; // channel 0x100A
        mac[4] = 0x00;
        mac[5] = 0x00;
        mac[6] = 0x01; // target
        mac[7] = 0x00;
        mac[8] = 0x00;
        mac[9] = 0x02; // source
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {iden, type, tdma, base, spac};
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("UU tuned", tuned == 1);
        rc |= expect_eq_long("UU vc", vc, 851125000);
    }

    // Case C: Group Voice Channel Grant Update Multiple - Explicit (opcode 0x25)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x25;
        mac[2] = 0x00; // svc1
        mac[3] = 0x10;
        mac[4] = 0x0A; // channel T1 0x100A
        mac[5] = 0x00;
        mac[6] = 0x00; // channel R1 unused
        mac[7] = 0x12;
        mac[8] = 0x34; // group1
        mac[9] = 0x00; // svc2
        mac[10] = 0x10;
        mac[11] = 0x0B; // channel T2 0x100B
        mac[12] = 0x00;
        mac[13] = 0x00; // channel R2 unused
        mac[14] = 0x56;
        mac[15] = 0x78; // group2
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {iden, type, tdma, base, spac};
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("0x25 tuned", tuned == 1);
        rc |= expect_eq_long("0x25 vc", vc, 851125000);
    }

    // Case D: SNDCP Data Channel Announcement resolves both T and R channels (opcode 0xD6)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0xD6;
        mac[4] = 0x10;
        mac[5] = 0x0A; // CHAN-T 0x100A
        mac[6] = 0x10;
        mac[7] = 0x0B; // CHAN-R 0x100B
        long freq_t = 0;
        long freq_r = 0;
        p25_test_iden_config cfg = {iden, type, tdma, base, spac};
        p25_test_invoke_mac_vpdu_channel_cache(mac, 24, &cfg, 0x100A, 0x100B, &freq_t, &freq_r);
        rc |= expect_eq_long("0xD6 CHAN-T cache", freq_t, 851125000);
        rc |= expect_eq_long("0xD6 CHAN-R cache", freq_r, 851137500);
    }

    // Case E: a grant decoded through MAC VPDU must honor failed-VC retune backoff.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;
        state.p25_retune_block_freq = 851125000;
        state.p25_retune_block_slot = -1;
        state.p25_retune_block_until = time(NULL) + 60;

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> blocked 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 blocked by backoff", opts.p25_is_tuned == 0);
        rc |= expect_eq_long("0x40 blocked vc", state.p25_vc_freq[0], 0);
    }

    // Case F: first live VPDU grant seeds an unknown CC from the current RTL tuner.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtlsdr_center_freq = (uint32_t)cc;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p2_is_lcch = 1;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 seeded CC tuned", opts.p25_is_tuned == 1);
        rc |= expect_eq_long("0x40 seeded CC", state.p25_cc_freq, cc);
        rc |= expect_eq_long("0x40 seeded trunk CC", state.trunk_cc_freq, cc);
        rc |= expect_true("0x40 seeded TDMA CC hint", state.p25_cc_is_tdma == 1);
        rc |= expect_eq_long("0x40 seeded vc", state.p25_vc_freq[0], 851125000);
    }

    // Case G: traffic-channel MACs must not learn the current VC tuner frequency as the CC.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtlsdr_center_freq = 852000000U;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p2_is_lcch = 0;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant carried on FACCH/SACCH traffic MAC
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 traffic MAC not tuned", opts.p25_is_tuned == 0);
        rc |= expect_eq_long("0x40 traffic MAC no p25 CC seed", state.p25_cc_freq, 0);
        rc |= expect_eq_long("0x40 traffic MAC no trunk CC seed", state.trunk_cc_freq, 0);
        rc |= expect_eq_long("0x40 traffic MAC no vc", state.p25_vc_freq[0], 0);
    }

    // Case H: first live VPDU grant can use the generic trunk CC alias when the P25 alias is still unknown.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        state.trunk_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 trunk alias CC tuned", opts.p25_is_tuned == 1);
        rc |= expect_eq_long("0x40 trunk alias p25 CC", state.p25_cc_freq, cc);
        rc |= expect_eq_long("0x40 trunk alias trunk CC", state.trunk_cc_freq, cc);
        rc |= expect_eq_long("0x40 trunk alias vc", state.p25_vc_freq[0], 851125000);
    }

    // Case I: compact grant-update paths must also honor failed-VC retune backoff.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;
        state.p25_retune_block_freq = 851125000;
        state.p25_retune_block_slot = -1;
        state.p25_retune_block_until = time(NULL) + 60;

        MAC[1] = 0x42; // Group Voice Channel Grant Update - Implicit
        MAC[2] = 0x10;
        MAC[3] = 0x0A; // channel1 0x100A -> blocked 851.125 MHz
        MAC[4] = 0x12;
        MAC[5] = 0x34; // group1
        MAC[6] = 0x10;
        MAC[7] = 0x0A; // channel2 same as channel1, so only one candidate is tried
        MAC[8] = 0x12;
        MAC[9] = 0x35; // group2

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x42 blocked by backoff", opts.p25_is_tuned == 0);
        rc |= expect_eq_long("0x42 blocked vc", state.p25_vc_freq[0], 0);
    }

    // Case J: rejected P2 NSBs still prove the system carries TDMA voice without changing return CC metadata.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_is_tuned = 1;
        state.p25_cc_freq = cc;
        state.trunk_cc_freq = cc;
        state.p25_cc_is_tdma = 0;
        state.p2_wacn = 0x11111;
        state.p2_sysid = 0x222;
        state.p2_cc = 0x333;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;

        MAC[1] = 0x7B; // Network Status Broadcast - Abbreviated
        MAC[2] = 0x01; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1; // WACN 0xABCDE, SYSID high nibble 0x1
        MAC[6] = 0x23; // SYSID low byte
        MAC[7] = 0x10;
        MAC[8] = 0x0A; // channel 0x100A -> 851.125 MHz, rejected while selected CC is 851 MHz
        MAC[9] = 0x00; // sysclass
        MAC[10] = 0x00;
        MAC[11] = 0x55; // NAC

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 rejected nsb sets system tdma hint", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 rejected nsb preserves cc modulation", state.p25_cc_is_tdma == 0);
        rc |= expect_eq_long("p2 rejected nsb preserves p25 cc", state.p25_cc_freq, cc);
        rc |= expect_eq_long("p2 rejected nsb preserves trunk cc", state.trunk_cc_freq, cc);
        rc |= expect_eq_long("p2 rejected nsb preserves wacn", (long)state.p2_wacn, 0x11111);
        rc |= expect_eq_long("p2 rejected nsb preserves sysid", (long)state.p2_sysid, 0x222);
        rc |= expect_eq_long("p2 rejected nsb preserves nac", (long)state.p2_cc, 0x333);
    }

    // Case K: P2 abbreviated NSB with unknown IDEN keeps identity metadata but
    // does not promote the unresolved channel to current CC or LCN0.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        MAC[1] = 0x7B;
        MAC[2] = 0x05; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1;
        MAC[6] = 0x23;
        MAC[7] = 0x80;
        MAC[8] = 0x0A; // unknown IDEN 8
        MAC[9] = 0x00;
        MAC[10] = 0x00;
        MAC[11] = 0x55;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 unknown-iden nsb marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 unknown-iden nsb does not mark cc tdma", state.p25_cc_is_tdma == 0);
        rc |= expect_eq_long("p2 unknown-iden nsb p25 cc empty", state.p25_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb trunk cc empty", state.trunk_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb lcn0 empty", state.trunk_lcn_freq[0], 0);
        rc |= expect_eq_long("p2 unknown-iden nsb wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 unknown-iden nsb sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 unknown-iden nsb nac", (long)state.p2_cc, 0x055);
        rc |= expect_eq_long("p2 unknown-iden nsb lra", state.p25_site_lra, 0x05);
        rc |= expect_eq_long("p2 unknown-iden nsb lra valid", state.p25_site_lra_valid, 1);
    }

    // Case L: P2 extended NSB with unknown IDEN keeps identity metadata but
    // does not promote the unresolved channel to current CC.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        MAC[1] = 0xFB;
        MAC[2] = 0x06; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1;
        MAC[6] = 0x23;
        MAC[7] = 0x80;
        MAC[8] = 0x0A; // CHAN-T unknown IDEN 8
        MAC[9] = 0x80;
        MAC[10] = 0x0B; // CHAN-R unknown IDEN 8
        MAC[12] = 0x00;
        MAC[13] = 0x56;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 unknown-iden nsb-ext marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 unknown-iden nsb-ext does not mark cc tdma", state.p25_cc_is_tdma == 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext p25 cc empty", state.p25_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext trunk cc empty", state.trunk_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext lcn0 empty", state.trunk_lcn_freq[0], 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext nac", (long)state.p2_cc, 0x056);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext lra", state.p25_site_lra, 0x06);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext lra valid", state.p25_site_lra_valid, 1);
    }

    // Case M: accepted P2 abbreviated NSB promotes the current CC to TDMA,
    // stores system identity, seeds LCN0, and confirms matching IDEN provenance.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 1;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_iden_fdma[iden].wacn = 0xABCDE;
        state.p25_iden_fdma[iden].sysid = 0x123;

        MAC[1] = 0x7B; // Network Status Broadcast - Abbreviated
        MAC[2] = 0x01; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1; // WACN 0xABCDE, SYSID high nibble 0x1
        MAC[6] = 0x23; // SYSID low byte
        MAC[7] = 0x10;
        MAC[8] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[9] = 0x00; // sysclass
        MAC[10] = 0x00;
        MAC[11] = 0x55; // NAC

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 accepted nsb marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 accepted nsb marks cc tdma", state.p25_cc_is_tdma == 1);
        rc |= expect_eq_long("p2 accepted nsb p25 cc", state.p25_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb trunk cc", state.trunk_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb lcn0", state.trunk_lcn_freq[0], 851125000);
        rc |= expect_eq_long("p2 accepted nsb wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 accepted nsb sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 accepted nsb nac", (long)state.p2_cc, 0x055);
        rc |= expect_eq_long("p2 accepted nsb confirms iden", state.p25_iden_fdma[iden].trust, 2);
    }

    // Case N: accepted P2 extended NSB resolves both T/R channels and updates
    // TDMA CC identity through the same state-machine notification path.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].populated = 1;

        MAC[1] = 0xFB; // Network Status Broadcast - Extended
        MAC[2] = 0x02; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1; // WACN 0xABCDE, SYSID high nibble 0x1
        MAC[6] = 0x23; // SYSID low byte
        MAC[7] = 0x10;
        MAC[8] = 0x0A; // CHAN-T 0x100A
        MAC[9] = 0x10;
        MAC[10] = 0x0B; // CHAN-R 0x100B
        MAC[12] = 0x00;
        MAC[13] = 0x56; // NAC

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 accepted nsb-ext marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 accepted nsb-ext marks cc tdma", state.p25_cc_is_tdma == 1);
        rc |= expect_eq_long("p2 accepted nsb-ext p25 cc", state.p25_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb-ext trunk cc", state.trunk_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb-ext chan-t cache", state.trunk_chan_map[0x100A], 851125000);
        rc |= expect_eq_long("p2 accepted nsb-ext chan-r cache", state.trunk_chan_map[0x100B], 851137500);
        rc |= expect_eq_long("p2 accepted nsb-ext wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 accepted nsb-ext sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 accepted nsb-ext nac", (long)state.p2_cc, 0x056);
    }

    // Case O: encrypted explicit multi-grants should publish channel state but
    // suppress retune when encrypted following is disabled and no clear key is known.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x25; // Group Voice Channel Grant Update Multiple - Explicit
        MAC[2] = 0x40; // encrypted svc1
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x00;
        MAC[6] = 0x00;
        MAC[7] = 0x12;
        MAC[8] = 0x34;
        MAC[9] = 0x40; // encrypted svc2
        MAC[10] = 0x10;
        MAC[11] = 0x0B;
        MAC[12] = 0x00;
        MAC[13] = 0x00;
        MAC[14] = 0x56;
        MAC[15] = 0x78;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x25 encrypted multi suppressed tune", opts.p25_is_tuned == 0);
        rc |= expect_eq_long("0x25 encrypted multi no vc", state.p25_vc_freq[0], 0);
        rc |= expect_contains("0x25 encrypted multi active ch1", state.active_channel[0], "Active Ch: 100A");
        rc |= expect_contains("0x25 encrypted multi active ch2", state.active_channel[0], "Ch: 100B");
        rc |= expect_contains("0x25 encrypted multi active ch group1", state.active_channel[0], "TG: 4660");
        rc |= expect_contains("0x25 encrypted multi active ch group2", state.active_channel[0], "TG: 22136");
    }

    // Case P: MAC words are specified as octets. If high bits leak into a
    // word, the VPDU decoder must mask them before rendering active channels.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x25; // Group Voice Channel Grant Update Multiple - Explicit
        MAC[2] = 0x00;
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x00;
        MAC[6] = 0x00;
        MAC[7] = 0xCE6387; // group high octet should become 0x87
        MAC[8] = 0x00;
        MAC[9] = 0x00;
        MAC[10] = 0xBF776F; // channel high octet should become 0x6F
        MAC[11] = 0x10;
        MAC[12] = 0x00;
        MAC[13] = 0x00;
        MAC[14] = 0x00;
        MAC[15] = 0x3E;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_contains("0x25 octet clamp active ch1", state.active_channel[0], "Active Ch: 100A");
        rc |= expect_contains("0x25 octet clamp active ch2", state.active_channel[0], "Ch: 6F10");
        rc |= expect_contains("0x25 octet clamp group1", state.active_channel[0], "TG: 34560");
        rc |= expect_contains("0x25 octet clamp group2", state.active_channel[0], "TG: 62");
    }

    // Case Q: encrypted implicit triple updates are also blocked before candidate
    // tuning while still refreshing active-channel state.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x05; // Group Voice Channel Grant Update Multiple - Implicit
        MAC[2] = 0x40;
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x12;
        MAC[6] = 0x34;
        MAC[7] = 0x40;
        MAC[8] = 0x10;
        MAC[9] = 0x0B;
        MAC[10] = 0x56;
        MAC[11] = 0x78;
        MAC[12] = 0x40;
        MAC[13] = 0x10;
        MAC[14] = 0x0C;
        MAC[15] = 0x9A;
        MAC[16] = 0xBC;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x05 encrypted triple suppressed tune", opts.p25_is_tuned == 0);
        rc |= expect_eq_long("0x05 encrypted triple no vc", state.p25_vc_freq[0], 0);
        rc |= expect_contains("0x05 encrypted triple active group1", state.active_channel[0], "TG: 4660");
        rc |= expect_contains("0x05 encrypted triple active group2", state.active_channel[0], "TG: 22136");
        rc |= expect_contains("0x05 encrypted triple active group3", state.active_channel[0], "TG: 39612");
    }

    // Case R: telephone interconnect grants carry service state and tune like
    // private calls when private-call following is enabled.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 1;
        opts.trunk_tune_private_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[0] = 0x07; // implicit telephone grant uses the unshifted VPDU layout
        MAC[1] = 0x48; // Telephone Interconnect Voice Channel Grant - implicit
        MAC[2] = 0x93; // emergency, packet, priority 3
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x00;
        MAC[6] = 0x2A; // timer
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05; // target

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x48 telephone no unsupported tune", opts.p25_is_tuned == 0);
        rc |= expect_eq_long("0x48 telephone no vc", state.p25_vc_freq[0], 0);
        rc |= expect_contains("0x48 telephone active", state.active_channel[0], "Active Tele Ch: 100A");
        rc |= expect_contains("0x48 telephone target", state.active_channel[0], "TGT: 197637");
        rc |= expect_eq_long("0x48 telephone svc", state.dmr_so, 0x93);
        rc |= expect_eq_long("0x48 telephone emergency", state.p25_call_emergency[0], 1);
        rc |= expect_eq_long("0x48 telephone packet", state.p25_call_is_packet[0], 1);
    }

    // Case S: explicit SNDCP data grants update data-channel state in playback
    // mode, with P25p2 playback mirroring both TDMA slots.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_on_release(&opts, &state);

        opts.p25_trunk = 0;
        state.lasttg = 0x030405;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x54; // SNDCP Data Channel Grant - explicit
        MAC[2] = 0x22; // DSO
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // CHAN-T
        MAC[5] = 0x10;
        MAC[6] = 0x0B; // CHAN-R
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05; // target

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_contains("0x54 data active", state.active_channel[0], "Active Data Ch: 100A");
        rc |= expect_contains("0x54 data target", state.active_channel[0], "TGT: 197637");
        rc |= expect_eq_long("0x54 data vc0", state.p25_vc_freq[0], 851125000);
        rc |= expect_eq_long("0x54 data vc1", state.p25_vc_freq[1], 851125000);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
