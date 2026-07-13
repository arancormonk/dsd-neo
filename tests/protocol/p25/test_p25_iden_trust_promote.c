// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 IDEN trust promotion tests.
 * Verifies that p25_confirm_idens_for_current_site promotes trust to 2 only
 * when provenance (WACN/SYSID and, if present, RFSS/SITE) matches current site.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

// Alias decode helpers referenced by linked modules.
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* o, dsd_state* s, uint8_t slot, uint8_t* b) {
    (void)o;
    (void)s;
    (void)slot;
    (void)b;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* o, dsd_state* s, uint8_t slot, uint8_t* b) {
    (void)o;
    (void)s;
    (void)slot;
    (void)b;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* o, dsd_state* s, uint8_t slot, int16_t len, uint8_t* in) {
    (void)o;
    (void)s;
    (void)slot;
    (void)len;
    (void)in;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* o, dsd_state* s, uint8_t* in, uint32_t src, int slot) {
    (void)o;
    (void)s;
    (void)in;
    (void)src;
    (void)slot;
}

int
main(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);

    // Current site identity
    st.p2_wacn = 0xABCDE;
    st.p2_sysid = 0x123;
    st.p2_rfssid = 4;
    st.p2_siteid = 7;

    // Case A: WACN/SYSID match; RFSS/SITE unset → promote to 2
    int idA = 1;
    st.p25_iden_fdma[idA].wacn = st.p2_wacn;
    st.p25_iden_fdma[idA].sysid = st.p2_sysid;
    st.p25_iden_fdma[idA].rfss = 0;
    st.p25_iden_fdma[idA].site = 0;
    st.p25_iden_fdma[idA].trust = 1; // seen but unconfirmed
    st.p25_iden_fdma[idA].populated = 1;

    // Case B: all match → promote to 2
    int idB = 2;
    st.p25_iden_fdma[idB].wacn = st.p2_wacn;
    st.p25_iden_fdma[idB].sysid = st.p2_sysid;
    st.p25_iden_fdma[idB].rfss = st.p2_rfssid;
    st.p25_iden_fdma[idB].site = st.p2_siteid;
    st.p25_iden_fdma[idB].trust = 1;
    st.p25_iden_fdma[idB].populated = 1;

    // Case C: RFSS mismatch → remain <2
    int idC = 3;
    st.p25_iden_fdma[idC].wacn = st.p2_wacn;
    st.p25_iden_fdma[idC].sysid = st.p2_sysid;
    st.p25_iden_fdma[idC].rfss = st.p2_rfssid + 1;
    st.p25_iden_fdma[idC].site = st.p2_siteid;
    st.p25_iden_fdma[idC].trust = 1;
    st.p25_iden_fdma[idC].populated = 1;

    // Case D: SITE mismatch → remain <2
    int idD = 4;
    st.p25_iden_fdma[idD].wacn = st.p2_wacn;
    st.p25_iden_fdma[idD].sysid = st.p2_sysid;
    st.p25_iden_fdma[idD].rfss = st.p2_rfssid;
    st.p25_iden_fdma[idD].site = st.p2_siteid + 1;
    st.p25_iden_fdma[idD].trust = 1;
    st.p25_iden_fdma[idD].populated = 1;

    p25_confirm_idens_for_current_site(&st);

    rc |= expect_eq_int("trust A", st.p25_iden_fdma[idA].trust, 2);
    rc |= expect_eq_int("trust B", st.p25_iden_fdma[idB].trust, 2);
    rc |= expect_eq_int("trust C", st.p25_iden_fdma[idC].trust == 2, 0);
    rc |= expect_eq_int("trust D", st.p25_iden_fdma[idD].trust == 2, 0);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
