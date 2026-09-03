// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 channel suffix formatting tests.
 * Ensures TDMA/FDMA suffix formatting and CC-TDMA fallback denom behavior.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);

    char buf[32];

    // Case 1: Known TDMA (type=3 -> denom=2). Raw ch=0x2005 -> FDMA 0x0002, slot 2
    int id = 2;
    // Populate new dual-array entry so p25_format_chan_suffix reads from it
    st.p25_iden_tdma[id].chan_type = 3;
    st.p25_iden_tdma[id].populated = 1;
    st.p25_chan_tdma_explicit[id] = 2; // TDMA known
    uint16_t ch = (uint16_t)((id << 12) | 0x0005);
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(&st, ch, -1, buf, sizeof buf);
    rc |= expect_eq_str("tdma suffix", buf, " (2-5) (FDMA 0002 S2)");
    // Override slot via hint
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(&st, ch, 0, buf, sizeof buf);
    rc |= expect_eq_str("tdma slot hint", buf, " (2-5) (FDMA 0002 S1)");

    // Case 2: FDMA (denom=1) → only the pasteable <iden>-<chan> key
    static dsd_state st2;
    DSD_MEMSET(&st2, 0, sizeof st2);
    st2.p25_chan_tdma_explicit[id] = 1; // FDMA known
    st2.p25_cc_is_tdma = 0;
    ch = (uint16_t)((id << 12) | 0x000A);
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(&st2, ch, -1, buf, sizeof buf);
    rc |= expect_eq_str("fdma suffix key only", buf, " (2-10)");

    // Case 3: System has Phase 2 TDMA voice but IDEN TDMA unknown → fallback denom=2
    static dsd_state st3;
    DSD_MEMSET(&st3, 0, sizeof st3);
    st3.p25_sys_is_tdma = 1;
    ch = (uint16_t)((id << 12) | 0x0007); // raw 7 -> FDMA 3, slot 2 (1-based)
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(&st3, ch, -1, buf, sizeof buf);
    rc |= expect_eq_str("fallback denom=2", buf, " (2-7) (FDMA 0003 S2)");

    // Case 4: Explicit FDMA must not show a TDMA suffix even if the system has
    // Phase 2 voice and the system-wide TDMA indication is set.
    static dsd_state st4;
    DSD_MEMSET(&st4, 0, sizeof st4);
    st4.p25_sys_is_tdma = 1;
    st4.p25_chan_tdma_explicit[id] = 1; // explicit FDMA
    // Populate FDMA entry (explicit=1 means FDMA-only, so FDMA entry is selected)
    st4.p25_iden_fdma[id].chan_type = 3;
    st4.p25_iden_fdma[id].populated = 1;
    ch = (uint16_t)((id << 12) | 0x000A);
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(&st4, ch, -1, buf, sizeof buf);
    rc |= expect_eq_str("explicit fdma suffix key only", buf, " (2-10)");

    // Case 5: the key is the raw 16-bit channel in DSDPlus notation, so 0x2A46 reads 2-2630 and
    // the widest key still fits the 32-byte buffers the call sites use.
    static dsd_state st5;
    DSD_MEMSET(&st5, 0, sizeof st5);
    st5.p25_chan_tdma_explicit[2] = 1;
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(&st5, (uint16_t)0x2A46, -1, buf, sizeof buf);
    rc |= expect_eq_str("hex 2A46 reads 2-2630", buf, " (2-2630)");
    st5.p25_iden_tdma[15].chan_type = 4;
    st5.p25_iden_tdma[15].populated = 1;
    st5.p25_chan_tdma_explicit[15] = 2;
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(&st5, (uint16_t)0xFFFE, -1, buf, sizeof buf);
    rc |= expect_eq_str("widest key fits", buf, " (15-4094) (FDMA 03FF S3)");
    // A NULL state still yields the key: it needs no IDEN knowledge.
    DSD_MEMSET(buf, 0, sizeof buf);
    p25_format_chan_suffix(NULL, (uint16_t)0x2A46, -1, buf, sizeof buf);
    rc |= expect_eq_str("null state key only", buf, " (2-2630)");

    return rc;
}
