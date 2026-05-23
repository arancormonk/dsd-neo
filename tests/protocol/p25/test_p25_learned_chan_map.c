// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 learned channel map persistence test.
 * Verifies that once a channel→freq is computed, it is stored in trunk_chan_map
 * and used even if IDEN base/spacing are cleared thereafter.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
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

// Stubs
bool
SetFreq(int sockfd, long int freq) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)bandwidth;
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
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    int iden = 1;
    int chan = (iden << 12) | 0x000A; // 0x100A
    // Populate new dual-array (process_channel_to_freq reads from these)
    st.p25_iden_fdma[iden].base_freq = 851000000 / 5;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].chan_spac = 100;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] = 1; // FDMA known

    long f1 = process_channel_to_freq(&opts, &st, chan);
    long want = 851000000 + 10 * 100 * 125; // 851.125 MHz
    rc |= expect_eq_long("first calc", f1, want);

    // Clear IDEN params; subsequent lookup should still return via trunk_chan_map
    st.p25_iden_fdma[iden].base_freq = 0;
    st.p25_iden_fdma[iden].chan_spac = 0;
    long f2 = process_channel_to_freq(&opts, &st, chan);
    rc |= expect_eq_long("map fallback", f2, want);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
