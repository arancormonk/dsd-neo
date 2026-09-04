// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_audio.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stddef.h>

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

double
rtl_stream_return_pwr(const RtlSdrContext* ctx) {
    ++g_rtl_return_pwr_calls;
    g_last_rtl_ctx = (RtlSdrContext*)ctx;
    return 12.25;
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

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
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

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
}

int
main(void) {
    test_udp_audio_installer();
    test_rtl_stream_io_installer();
    return 0;
}
