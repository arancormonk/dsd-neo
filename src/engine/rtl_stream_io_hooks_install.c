// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/io/rtl_stream_fwd.h"
#include "engine_hooks_install.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>

static int
rtl_stream_io_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return rtl_stream_read((RtlSdrContext*)rtl_ctx, out, count, out_got);
}

static int
rtl_stream_io_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    return rtl_stream_read_ex((RtlSdrContext*)rtl_ctx, out, flags, count, out_got);
}

static int
rtl_stream_io_squelch_status(const void* rtl_ctx, dsd_rtl_squelch_status* out) {
    (void)rtl_ctx;
    rtl_stream_squelch_status st;
    if (rtl_stream_get_squelch_status(&st) != 0) {
        return -1;
    }
    out->active = st.active;
    out->noise = st.noise;
    out->quieting_db = st.quieting_db;
    out->state = st.state;
    out->gate_open = st.gate_open;
    out->plan_valid = st.plan_valid;
    out->floor_power = st.floor_power;
    out->window_power = st.window_power;
    return 0;
}

static double
rtl_stream_io_return_pwr(const void* rtl_ctx) {
    return rtl_stream_return_pwr((const RtlSdrContext*)rtl_ctx);
}
#endif

void
dsd_engine_rtl_stream_io_hooks_install(void) {
    dsd_rtl_stream_io_hooks hooks = {0};
#ifdef USE_RADIO
    hooks.read = rtl_stream_io_read;
    hooks.return_pwr = rtl_stream_io_return_pwr;
    hooks.read_ex = rtl_stream_io_read_ex;
    hooks.squelch_status = rtl_stream_io_squelch_status;
#endif
    dsd_rtl_stream_io_hooks_set(hooks);
}
