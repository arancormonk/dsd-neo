// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Control-channel cache path formatting tests.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#undef main

#include <dsd-neo/protocol/p25/p25_cc_candidates.h>

static int
expect_eq_str(const char* tag, const char* a, const char* b) {
    if (strcmp(a ? a : "", b ? b : "") != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", tag, a ? a : "(null)", b ? b : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    char tmpl[] = "/tmp/dsdneo_cc_path_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 100;
    }
    setenv("DSD_NEO_CACHE_DIR", dir, 1);

    dsd_state st;
    memset(&st, 0, sizeof st);

    // No identity -> no path
    char out[1024];
    int ok = p25_cc_build_cache_path(&st, out, sizeof out);
    rc |= expect_eq_int("no identity", ok, 0);

    // With WACN/SYSID only
    st.p2_wacn = 0xABCDEULL;
    st.p2_sysid = 0x123ULL;
    ok = p25_cc_build_cache_path(&st, out, sizeof out);
    rc |= expect_eq_int("iden only ok", ok, 1);
    char want1[1024];
    snprintf(want1, sizeof want1, "%s/p25_cc_%05lX_%03lX.txt", dir, (unsigned long)st.p2_wacn,
             (unsigned long)st.p2_sysid);
    rc |= expect_eq_str("iden only path", out, want1);

    // With RFSS/SITE
    st.p2_rfssid = 7ULL;
    st.p2_siteid = 11ULL;
    ok = p25_cc_build_cache_path(&st, out, sizeof out);
    rc |= expect_eq_int("rfss/site ok", ok, 1);
    char want2[1024];
    snprintf(want2, sizeof want2, "%s/p25_cc_%05lX_%03lX_R%03llu_S%03llu.txt", dir, (unsigned long)st.p2_wacn,
             (unsigned long)st.p2_sysid, st.p2_rfssid, st.p2_siteid);
    rc |= expect_eq_str("rfss/site path", out, want2);

    return rc;
}
