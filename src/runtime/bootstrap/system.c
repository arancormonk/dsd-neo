// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/log.h>

#include <stdlib.h>

#if defined(__SSE__) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

static int
dsd_truthy_env(const char* v) {
    if (!v || !*v) {
        return 0;
    }
    if (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T') {
        return 1;
    }
    return 0;
}

void
dsd_bootstrap_enable_ftz_daz_if_enabled(void) {
#if defined(__SSE__) || defined(__SSE2__)
    const char* e = getenv("DSD_NEO_FTZ_DAZ");
    if (!dsd_truthy_env(e)) {
        return;
    }
    unsigned int mxcsr = _mm_getcsr();
    mxcsr |= (1u << 15) | (1u << 6); // FTZ | DAZ
    _mm_setcsr(mxcsr);
    LOG_NOTICE("Enabled SSE FTZ/DAZ (env DSD_NEO_FTZ_DAZ)\n");
#endif
}
