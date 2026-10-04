// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_audio.h>
#include <dsd-neo/io/udp_socket_connect.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "src/engine/engine_hooks_install.h"

static int g_udp_blast_calls = 0;
static int g_udp_analog_calls = 0;
static const dsd_opts* g_last_opts = NULL;
static dsd_state* g_last_state = NULL;
static size_t g_last_nsam = 0;
static const void* g_last_data = NULL;

static int g_rtl_read_calls = 0;
static int g_rtl_return_pwr_calls = 0;
static RtlSdrContext* g_last_rtl_ctx = NULL;
static size_t g_last_rtl_count = 0;

void
udp_socket_blaster(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    ++g_udp_blast_calls;
    g_last_opts = opts;
    g_last_state = state;
    g_last_nsam = nsam;
    g_last_data = data;
}

void
udp_socket_blasterA(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    ++g_udp_analog_calls;
    g_last_opts = opts;
    g_last_state = state;
    g_last_nsam = nsam;
    g_last_data = data;
}

static int g_udp_connect_analog_calls = 0;
static int g_udp_connect_analog_result = 0;

int
udp_socket_connectA(dsd_opts* opts, dsd_state* state) {
    (void)state;
    ++g_udp_connect_analog_calls;
    if (g_udp_connect_analog_result == 0) {
        opts->udp_sockfdA = (dsd_socket_t)5;
    }
    return g_udp_connect_analog_result;
}

int
rtl_stream_read(RtlSdrContext* ctx, float* out, size_t count, int* out_got) {
    ++g_rtl_read_calls;
    g_last_rtl_ctx = ctx;
    g_last_rtl_count = count;
    if (out != NULL && count > 0U) {
        out[0] = 9.5f;
    }
    if (out_got != NULL) {
        *out_got = (count > 0U) ? 1 : 0;
    }
    return 7;
}

static int g_rtl_read_ex_calls = 0;

int
rtl_stream_read_ex(RtlSdrContext* ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    ++g_rtl_read_ex_calls;
    g_last_rtl_ctx = ctx;
    g_last_rtl_count = count;
    if (out != NULL && count > 0U) {
        out[0] = 4.5f;
    }
    if (flags != NULL && count > 0U) {
        flags[0] = 1U;
    }
    if (out_got != NULL) {
        *out_got = (count > 0U) ? 1 : 0;
    }
    return 5;
}

int
rtl_stream_get_squelch_status(rtl_stream_squelch_status* out) {
    out->active = 1;
    out->noise = 1;
    out->quieting_db = 23.456;
    out->state = 1;
    out->gate_open = 0;
    out->plan_valid = 1;
    out->floor_power = 2e-6;
    out->window_power = 3e-6;
    return 0;
}

double
rtl_stream_return_pwr(const RtlSdrContext* ctx) {
    ++g_rtl_return_pwr_calls;
    g_last_rtl_ctx = (RtlSdrContext*)ctx;
    return 12.25;
}

static int
rtl_stream_io_test_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return rtl_stream_read((RtlSdrContext*)rtl_ctx, out, count, out_got);
}

static void
reset_udp_stub(void) {
    g_udp_blast_calls = 0;
    g_udp_analog_calls = 0;
    g_last_opts = NULL;
    g_last_state = NULL;
    g_last_nsam = 0;
    g_last_data = NULL;
}

static void
reset_rtl_stub(void) {
    g_rtl_read_calls = 0;
    g_rtl_read_ex_calls = 0;
    g_rtl_return_pwr_calls = 0;
    g_last_rtl_ctx = NULL;
    g_last_rtl_count = 0;
}

static void
test_udp_audio_installer(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    unsigned char data[4] = {1, 2, 3, 4};

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_engine_udp_audio_hooks_install();

    reset_udp_stub();
    dsd_udp_audio_hook_blast(&opts, &state, 4U, data);
    assert(g_udp_blast_calls == 1);
    assert(g_udp_analog_calls == 0);
    assert(g_last_opts == &opts);
    assert(g_last_state == &state);
    assert(g_last_nsam == 4U);
    assert(g_last_data == data);

    dsd_udp_audio_hook_blast_analog(&opts, &state, 2U, data);
    assert(g_udp_blast_calls == 1);
    assert(g_udp_analog_calls == 1);
    assert(g_last_opts == &opts);
    assert(g_last_state == &state);
    assert(g_last_nsam == 2U);
    assert(g_last_data == data);

    /* The analog socket a runtime switch to the analog family opens lazily. */
    opts.udp_sockfdA = DSD_INVALID_SOCKET;
    g_udp_connect_analog_calls = 0;
    g_udp_connect_analog_result = 0;
    assert(dsd_udp_audio_hook_connect_analog(&opts) == 0);
    assert(g_udp_connect_analog_calls == 1);
    assert(opts.udp_sockfdA == (dsd_socket_t)5);
    opts.udp_sockfdA = DSD_INVALID_SOCKET;
    g_udp_connect_analog_result = -1;
    assert(dsd_udp_audio_hook_connect_analog(&opts) == -1);
    assert(g_udp_connect_analog_calls == 2);
    assert(opts.udp_sockfdA == DSD_INVALID_SOCKET);
    assert(dsd_udp_audio_hook_connect_analog(NULL) == -1);
    assert(g_udp_connect_analog_calls == 2);

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    assert(dsd_udp_audio_hook_connect_analog(&opts) == -1);
}

static void
test_rtl_stream_io_installer(void) {
    static dsd_state state = {0};
    static RtlSdrContext* fake_ctx = (RtlSdrContext*)0x1234;
    float sample = 0.0f;
    int got = 0;

    state.rtl_ctx = fake_ctx;
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_engine_rtl_stream_io_hooks_install();

    reset_rtl_stub();
    assert(dsd_rtl_stream_io_hook_read(&state, &sample, 3U, &got) == 7);
    assert(g_rtl_read_calls == 1);
    assert(g_last_rtl_ctx == fake_ctx);
    assert(g_last_rtl_count == 3U);
    assert(got == 1);
    assert(sample == 9.5f);

    assert(dsd_rtl_stream_io_hook_return_pwr(&state) == 12.25);
    assert(g_rtl_return_pwr_calls == 1);
    assert(g_last_rtl_ctx == fake_ctx);

    /* The flagged read goes to rtl_stream_read_ex() with the flags. */
    uint8_t flag = 0U;
    got = 0;
    assert(dsd_rtl_stream_io_hook_read_ex(&state, &sample, &flag, 2U, &got) == 5);
    assert(g_rtl_read_ex_calls == 1 && g_rtl_read_calls == 1);
    assert(g_last_rtl_count == 2U && got == 1 && sample == 4.5f && flag == 1U);

    /* The dynamic squelch's status reaches the runtime table, and dsd_state through the publication. */
    dsd_rtl_squelch_status status;
    assert(dsd_rtl_stream_io_hook_squelch_status(&state, &status) == 0);
    assert(status.active == 1 && status.state == 1 && status.gate_open == 0 && status.plan_valid == 1);
    assert(status.floor_power > 1.9e-6 && status.floor_power < 2.1e-6);
    assert(status.noise == 1 && status.quieting_db > 23.45 && status.quieting_db < 23.46);
    dsd_squelch_publish_status(&state);
    assert(state.squelch_auto_active == 1U && state.squelch_auto_state == 1U && state.squelch_auto_gate_open == 0U);
    /* 2e-6 / 2 = 1e-6: -60 dB. */
    assert(state.squelch_auto_floor_cdb == -6000);
    /* The noise squelch's quieting in hundredths of a dB. */
    assert(state.squelch_noise_active == 1U && state.squelch_noise_quieting_cdb == 2346);

    /* A host that installs read alone: read_ex reads through it, every flag open. */
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){.read = rtl_stream_io_test_read});
    assert(dsd_rtl_stream_io_hook_squelch_status(&state, &status) == -1 && status.active == 0);
    dsd_squelch_publish_status(&state);
    assert(state.squelch_auto_active == 0U && state.squelch_auto_floor_cdb == 0);
    assert(state.squelch_noise_active == 0U && state.squelch_noise_quieting_cdb == 0);
    flag = 7U;
    got = 0;
    assert(dsd_rtl_stream_io_hook_read_ex(&state, &sample, &flag, 1U, &got) == 7);
    assert(g_rtl_read_calls == 2 && got == 1 && sample == 9.5f && flag == 0U);
    assert(dsd_rtl_stream_io_hook_read_ex(&state, &sample, NULL, 1U, &got) == 7);
    assert(dsd_rtl_stream_io_hook_read_ex(&state, NULL, &flag, 1U, &got) == -1);
    assert(dsd_rtl_stream_io_hook_read_ex(&state, &sample, &flag, 0U, &got) == -1);

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
}

int
main(void) {
    test_udp_audio_installer();
    test_rtl_stream_io_installer();
    return 0;
}
