// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The auto squelch's plumbing in the RTL stream (issue #518 follow-up): the setting a control thread pushes reaches the
 * demod thread whole, with the level gate out of force under AUTO; the demod thread's floor context; samples and their
 * gate flags travelling together through the output ring and both reads; the published status.
 */

#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

int
main(void) {
    const int failed = rtl_stream_test_squelch_plumbing();
    if (failed != 0) {
        DSD_FPRINTF(stderr, "IO_RTL_SQUELCH_PLUMBING: check %d failed (rtl_stream_test_squelch_plumbing())\n", failed);
        return 1;
    }
    printf("IO_RTL_SQUELCH_PLUMBING: OK\n");
    return 0;
}
