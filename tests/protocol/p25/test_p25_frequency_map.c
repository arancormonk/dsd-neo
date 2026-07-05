// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 channel→frequency mapping tests (FDMA/TDMA + overrides).
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Use shim to avoid pulling in external deps like mbelib.
int p25_test_frequency_for(int iden, int type, int tdma, long base, int spac, int chan16, long map_override,
                           long* out_freq);

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output,
                                 int len) { // NOLINT(misc-use-internal-linkage)
    (void)input;
    (void)output;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                 uint8_t* lc_bits) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                 uint8_t* lc_bits) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len,
                          uint8_t* input) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src,
            int slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

bool
SetFreq(int sockfd, long int freq) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bw) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)bw;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}
struct RtlSdrContext* g_rtl_ctx = 0; // NOLINT(misc-use-internal-linkage)

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) { // NOLINT(misc-use-internal-linkage)
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
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
expect_eq_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    // Case 1: FDMA mapping
    {
        long f0 = 0, fA = 0;
        p25_test_frequency_for(1, /*type*/ 1, /*tdma*/ 0, 851000000 / 5, 100, 0x1000, 0, &f0);
        p25_test_frequency_for(1, /*type*/ 1, /*tdma*/ 0, 851000000 / 5, 100, 0x100A, 0, &fA);
        rc |= expect_eq_long("FDMA ch=0", f0, 851000000);
        rc |= expect_eq_long("FDMA ch=10", fA, 851000000 + 10 * 100 * 125);
    }

    // Case 1b: full channel value 0x0000 is valid when IDEN 0 exists.
    {
        long f = 0;
        p25_test_frequency_for(0, /*type*/ 1, /*tdma*/ 0, 769000000 / 5, 100, 0x0000, 0, &f);
        rc |= expect_eq_long("FDMA full channel zero", f, 769000000);
    }

    // Case 2: TDMA mapping with slots-per-carrier division (type=4 => denom=4)
    {
        long f = 0;
        p25_test_frequency_for(2, /*type*/ 4, /*tdma*/ 1, 935000000 / 5, 100, 0x2004, 0, &f);
        rc |= expect_eq_long("TDMA type4 ch=4", f, 935000000 + 1 * 100 * 125);
    }

    // Case 3: trunk_chan_map override
    {
        long f = 0;
        p25_test_frequency_for(1, /*type*/ 1, /*tdma*/ 0, 851000000 / 5, 100, 0x1005, 762000000, &f);
        rc |= expect_eq_long("map override", f, 762000000);
    }

    // Public trace and guard behavior.
    {
        p25_freq_trace_t trace;
        DSD_MEMSET(&trace, 0xA5, sizeof(trace));
        rc |= expect_eq_long("trace missing state", process_channel_to_freq_trace(NULL, NULL, 0x1000, &trace), 0);
        rc |= expect_eq_str("trace missing state source", trace.source, "invalid-state");
        rc |= expect_eq_str("trace missing state failure", trace.failure, "missing-state");

        static dsd_state st;
        DSD_MEMSET(&st, 0, sizeof(st));
        rc |= expect_eq_long("trace sentinel channel", process_channel_to_freq_trace(NULL, &st, 0xFFFF, &trace), 0);
        rc |= expect_eq_str("trace sentinel source", trace.source, "invalid-channel");
        rc |= expect_eq_str("trace sentinel failure", trace.failure, "sentinel-channel");

        rc |= expect_eq_long("trace missing iden", process_channel_to_freq_trace(NULL, &st, 0x1001, &trace), 0);
        rc |= expect_eq_str("trace missing iden source", trace.source, "missing-iden-fdma");
        rc |= expect_eq_str("trace missing iden failure", trace.failure, "missing-iden-params");

        dsd_state_set_trunk_chan_freq(&st, 0x1001U, 765432100L);
        p25_invalidate_chan_map_for_iden(NULL, 1);
        p25_invalidate_chan_map_for_iden(&st, -1);
        p25_invalidate_chan_map_for_iden(&st, 16);
        rc |= expect_eq_long("invalid iden did not clear map", st.trunk_chan_map[0x1001], 765432100L);
        p25_invalidate_chan_map_for_iden(&st, 1);
        rc |= expect_eq_long("valid iden clears map", st.trunk_chan_map[0x1001], 0);

        st.p25_chan_tdma_explicit[3] = 2;
        st.p25_iden_fdma[3].populated = 1;
        st.p25_iden_tdma[3].populated = 1;
        st.p25_secondary_cc_count = 1;
        st.p25_secondary_cc_entries[0].freq = 851012500L;
        st.p25_secondary_cc_entries[0].channel = 0x3001;
        st.p25_pending_announcement_count = 1;
        st.p25_pending_announcements[0].populated = 1;
        st.p25_pending_announcements[0].channel = 0x3001;
        p25_reset_iden_tables(NULL);
        p25_reset_iden_tables(&st);
        rc |= expect_eq_int("reset explicit tdma", st.p25_chan_tdma_explicit[3], 0);
        rc |= expect_eq_int("reset fdma populated", st.p25_iden_fdma[3].populated, 0);
        rc |= expect_eq_int("reset tdma populated", st.p25_iden_tdma[3].populated, 0);
        rc |= expect_eq_int("reset secondary cc count", st.p25_secondary_cc_count, 0);
        rc |= expect_eq_long("reset secondary cc freq", st.p25_secondary_cc_entries[0].freq, 0);
        rc |= expect_eq_int("reset pending count", st.p25_pending_announcement_count, 0);
        rc |= expect_eq_int("reset pending populated", st.p25_pending_announcements[0].populated, 0);

        st.p2_wacn = 0x11111;
        st.p2_sysid = 0x222;
        st.p25_chan_tdma_explicit[4] = 1;
        st.p25_iden_fdma[4].populated = 1;
        st.p25_secondary_cc_count = 1;
        st.p25_secondary_cc_entries[0].freq = 851012500L;
        st.p25_secondary_cc_entries[0].channel = 0x4001;
        st.p25_pending_announcement_count = 1;
        st.p25_pending_announcements[0].populated = 1;
        st.p25_pending_announcements[0].channel = 0x4001;
        dsd_state_set_trunk_chan_freq(&st, 0x0000U, 769000000L);
        dsd_state_set_trunk_chan_freq(&st, 0x4001U, 851012500L);
        uint64_t identity_map_seq = st.trunk_chan_map_seq;
        rc |= expect_eq_int("identity update applied", p25_update_system_identity(&st, 0xABCDE, 0x123), 1);
        rc |= expect_eq_long("identity update wacn", (long)st.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("identity update sysid", (long)st.p2_sysid, 0x123);
        rc |= expect_eq_int("identity update clears iden", st.p25_iden_fdma[4].populated, 0);
        rc |= expect_eq_int("identity update clears pending", st.p25_pending_announcement_count, 0);
        rc |= expect_eq_int("identity update clears secondary ccs", st.p25_secondary_cc_count, 0);
        rc |= expect_eq_long("identity update clears chan0 cache", st.trunk_chan_map[0x0000], 0);
        rc |= expect_eq_long("identity update clears chan cache", st.trunk_chan_map[0x4001], 0);
        rc |= expect_eq_int("identity update clears chan used", (int)st.trunk_chan_map_used_count, 0);
        int identity_map_seq_advanced = st.trunk_chan_map_seq > identity_map_seq ? 1 : 0;
        rc |= expect_eq_int("identity update advances chan map seq", identity_map_seq_advanced, 1);

        static dsd_opts opts;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        opts.verbose = 2;
        st.p25_iden_fdma[1].populated = 1;
        st.p25_iden_fdma[1].chan_type = 1;
        st.p25_iden_fdma[1].base_freq = 851000000 / 5;
        st.p25_iden_fdma[1].chan_spac = 100;
        rc |= expect_eq_long("verbose computed p25", process_channel_to_freq(&opts, &st, 0x1002),
                             851000000 + 2 * 100 * 125);
    }

    // NXDN channel mapping uses the same module and should cover DFA/cache/no-map paths.
    {
        static dsd_state ns;
        static dsd_opts opts;
        DSD_MEMSET(&ns, 0, sizeof(ns));
        DSD_MEMSET(&opts, 0, sizeof(opts));

        dsd_state_set_trunk_chan_freq(&ns, 10U, 123456789L);
        rc |= expect_eq_long("nxdn cache verbose", nxdn_channel_to_frequency(&opts, &ns, 10U), 123456789L);
        rc |= expect_eq_long("nxdn cache quiet", nxdn_channel_to_frequency_quiet(&ns, 10U), 123456789L);
        rc |= expect_eq_long("nxdn quiet null state", nxdn_channel_to_frequency_quiet(NULL, 10U), 0);

        DSD_MEMSET(&ns, 0, sizeof(ns));
        rc |= expect_eq_long("nxdn no rcn verbose", nxdn_channel_to_frequency(&opts, &ns, 10U), 0);
        rc |= expect_eq_long("nxdn no rcn quiet", nxdn_channel_to_frequency_quiet(&ns, 10U), 0);

        ns.nxdn_rcn = 1;
        ns.nxdn_base_freq = 1;
        ns.nxdn_step = 2;
        rc |= expect_eq_long("nxdn dfa base1 step2 verbose", nxdn_channel_to_frequency(&opts, &ns, 10U), 100012500L);

        DSD_MEMSET(&ns, 0, sizeof(ns));
        ns.nxdn_rcn = 1;
        ns.nxdn_base_freq = 2;
        ns.nxdn_step = 3;
        rc |= expect_eq_long("nxdn dfa base2 step3 quiet", nxdn_channel_to_frequency_quiet(&ns, 10U), 330031250L);

        DSD_MEMSET(&ns, 0, sizeof(ns));
        ns.nxdn_rcn = 1;
        ns.nxdn_base_freq = 3;
        ns.nxdn_step = 2;
        rc |= expect_eq_long("nxdn dfa base3 quiet", nxdn_channel_to_frequency_quiet(&ns, 10U), 400012500L);

        DSD_MEMSET(&ns, 0, sizeof(ns));
        ns.nxdn_rcn = 1;
        ns.nxdn_base_freq = 4;
        ns.nxdn_step = 3;
        rc |= expect_eq_long("nxdn dfa base4 quiet", nxdn_channel_to_frequency_quiet(&ns, 10U), 750031250L);

        DSD_MEMSET(&ns, 0, sizeof(ns));
        ns.nxdn_rcn = 1;
        ns.nxdn_base_freq = 99;
        ns.nxdn_step = 99;
        rc |= expect_eq_long("nxdn dfa unknown verbose", nxdn_channel_to_frequency(&opts, &ns, 10U), 0);
        rc |= expect_eq_long("nxdn dfa unknown quiet", nxdn_channel_to_frequency_quiet(&ns, 10U), 0);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
