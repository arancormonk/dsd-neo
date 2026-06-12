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
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

    // Case F: compact grant-update paths must also honor failed-VC retune backoff.
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

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
