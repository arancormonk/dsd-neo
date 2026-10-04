// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

static dsd_rtl_stream_io_hooks g_rtl_stream_io_hooks = {0};

void
dsd_rtl_stream_io_hooks_set(dsd_rtl_stream_io_hooks hooks) {
    g_rtl_stream_io_hooks = hooks;
}

int
dsd_rtl_stream_io_hook_read(dsd_state* state, float* out, size_t count, int* out_got) {
    int got_tmp = 0;
    int* out_got_ptr = out_got ? out_got : &got_tmp;
    *out_got_ptr = 0;

    if (!state || !state->rtl_ctx) {
        return -1;
    }
    if (!g_rtl_stream_io_hooks.read || !out || count == 0) {
        return -1;
    }

    return g_rtl_stream_io_hooks.read((void*)state->rtl_ctx, out, count, out_got_ptr);
}

int
dsd_rtl_stream_io_hook_read_ex(dsd_state* state, float* out, uint8_t* flags, size_t count, int* out_got) {
    int got_tmp = 0;
    int* out_got_ptr = out_got ? out_got : &got_tmp;
    *out_got_ptr = 0;

    if (!state || !state->rtl_ctx || !out || count == 0) {
        return -1;
    }
    if (g_rtl_stream_io_hooks.read_ex) {
        return g_rtl_stream_io_hooks.read_ex((void*)state->rtl_ctx, out, flags, count, out_got_ptr);
    }
    const int rc = dsd_rtl_stream_io_hook_read(state, out, count, out_got_ptr);
    if (flags && *out_got_ptr > 0) {
        DSD_MEMSET(flags, 0, (size_t)*out_got_ptr * sizeof(uint8_t));
    }
    return rc;
}

double
dsd_rtl_stream_io_hook_return_pwr(const dsd_state* state) {
    if (!state || !state->rtl_ctx) {
        return 0.0;
    }
    if (!g_rtl_stream_io_hooks.return_pwr) {
        return 0.0;
    }

    return g_rtl_stream_io_hooks.return_pwr((const void*)state->rtl_ctx);
}

int
dsd_rtl_stream_io_hook_squelch_status(const dsd_state* state, dsd_rtl_squelch_status* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!state || !state->rtl_ctx || !g_rtl_stream_io_hooks.squelch_status) {
        return -1;
    }
    return g_rtl_stream_io_hooks.squelch_status((const void*)state->rtl_ctx, out);
}
