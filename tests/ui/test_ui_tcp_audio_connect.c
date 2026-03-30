// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/ui/menu_services.h>

#include "dsd-neo/core/opts_fwd.h"

static dsd_socket_t g_connect_result = DSD_INVALID_SOCKET;
static int g_connect_calls = 0;
static char g_last_connect_host[256];
static int g_last_connect_port = 0;

static tcp_input_ctx* g_tcp_open_result = NULL;
static int g_tcp_open_calls = 0;
static dsd_socket_t g_last_tcp_open_sockfd = DSD_INVALID_SOCKET;
static int g_last_tcp_open_samplerate = 0;

static int g_tcp_close_calls = 0;
static tcp_input_ctx* g_last_tcp_close_ctx = NULL;

static int g_close_audio_input_calls = 0;

static int g_socket_close_calls = 0;
static dsd_socket_t g_closed_sockets[8];

static int g_resampler_reset_calls = 0;

static void
reset_fakes(void) {
    g_connect_result = DSD_INVALID_SOCKET;
    g_connect_calls = 0;
    g_last_connect_host[0] = '\0';
    g_last_connect_port = 0;
    g_tcp_open_result = NULL;
    g_tcp_open_calls = 0;
    g_last_tcp_open_sockfd = DSD_INVALID_SOCKET;
    g_last_tcp_open_samplerate = 0;
    g_tcp_close_calls = 0;
    g_last_tcp_close_ctx = NULL;
    g_close_audio_input_calls = 0;
    g_socket_close_calls = 0;
    memset(g_closed_sockets, 0, sizeof(g_closed_sockets));
    g_resampler_reset_calls = 0;
}

dsd_socket_t
Connect(char* hostname, int portno) {
    g_connect_calls++;
    snprintf(g_last_connect_host, sizeof(g_last_connect_host), "%s", hostname ? hostname : "");
    g_last_connect_port = portno;
    return g_connect_result;
}

tcp_input_ctx*
tcp_input_open(dsd_socket_t sockfd, int samplerate) {
    g_tcp_open_calls++;
    g_last_tcp_open_sockfd = sockfd;
    g_last_tcp_open_samplerate = samplerate;
    return g_tcp_open_result;
}

void
tcp_input_close(tcp_input_ctx* ctx) {
    g_tcp_close_calls++;
    g_last_tcp_close_ctx = ctx;
}

void
closeAudioInput(dsd_opts* opts) {
    (void)opts;
    g_close_audio_input_calls++;
}

int
dsd_socket_close(dsd_socket_t sockfd) {
    if (g_socket_close_calls < (int)(sizeof(g_closed_sockets) / sizeof(g_closed_sockets[0]))) {
        g_closed_sockets[g_socket_close_calls] = sockfd;
    }
    g_socket_close_calls++;
    return 0;
}

void
dsd_resampler_reset(dsd_resampler_state* state) {
    g_resampler_reset_calls++;
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->L = 1;
    state->M = 1;
}

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

static void
test_preserves_wav_input_when_tcp_backend_open_fails(void) {
    reset_fakes();

    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    snprintf(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "/tmp/current.wav");
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.audio_out_type = 0;
    opts.wav_sample_rate = 24000;
    opts.tcp_portno = 7355;
    snprintf(opts.tcp_hostname, sizeof(opts.tcp_hostname), "%s", "last.example");

    g_connect_result = (dsd_socket_t)77;
    g_tcp_open_result = NULL;

    assert(svc_tcp_connect_audio(&opts, "127.0.0.1", 9000) == -1);
    assert(opts.audio_in_type == AUDIO_IN_WAV);
    assert(strcmp(opts.audio_in_dev, "/tmp/current.wav") == 0);
    assert(opts.tcp_sockfd == 0);
    assert(opts.tcp_in_ctx == NULL);
    assert(strcmp(opts.tcp_hostname, "last.example") == 0);
    assert(opts.tcp_portno == 7355);
    assert(g_connect_calls == 1);
    assert(strcmp(g_last_connect_host, "127.0.0.1") == 0);
    assert(g_last_connect_port == 9000);
    assert(g_tcp_open_calls == 1);
    assert(g_close_audio_input_calls == 0);
    assert(g_tcp_close_calls == 0);
    assert(g_socket_close_calls == 1);
    assert(g_closed_sockets[0] == (dsd_socket_t)77);
    assert(g_resampler_reset_calls == 0);
}

static void
test_preserves_existing_tcp_input_when_reopen_fails(void) {
    reset_fakes();

    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 8000;
    opts.tcp_sockfd = (dsd_socket_t)12;
    opts.tcp_in_ctx = (tcp_input_ctx*)(uintptr_t)0x1234;
    opts.tcp_portno = 1200;
    snprintf(opts.tcp_hostname, sizeof(opts.tcp_hostname), "%s", "old.example");
    snprintf(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "tcp:old.example:1200");

    g_connect_result = (dsd_socket_t)99;
    g_tcp_open_result = NULL;

    assert(svc_tcp_connect_audio(&opts, "new.example", 1300) == -1);
    assert(opts.audio_in_type == AUDIO_IN_TCP);
    assert(opts.tcp_sockfd == (dsd_socket_t)12);
    assert(opts.tcp_in_ctx == (tcp_input_ctx*)(uintptr_t)0x1234);
    assert(strcmp(opts.audio_in_dev, "tcp:old.example:1200") == 0);
    assert(strcmp(opts.tcp_hostname, "old.example") == 0);
    assert(opts.tcp_portno == 1200);
    assert(g_connect_calls == 1);
    assert(strcmp(g_last_connect_host, "new.example") == 0);
    assert(g_last_connect_port == 1300);
    assert(g_tcp_open_calls == 1);
    assert(g_close_audio_input_calls == 0);
    assert(g_tcp_close_calls == 0);
    assert(g_socket_close_calls == 1);
    assert(g_closed_sockets[0] == (dsd_socket_t)99);
    assert(g_resampler_reset_calls == 0);
}

static void
test_preserves_existing_tcp_input_when_reconnect_fails_before_open(void) {
    reset_fakes();

    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 8000;
    opts.tcp_sockfd = (dsd_socket_t)12;
    opts.tcp_in_ctx = (tcp_input_ctx*)(uintptr_t)0x1234;
    opts.tcp_portno = 1200;
    snprintf(opts.tcp_hostname, sizeof(opts.tcp_hostname), "%s", "old.example");
    snprintf(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "tcp:old.example:1200");

    g_connect_result = DSD_INVALID_SOCKET;
    g_tcp_open_result = (tcp_input_ctx*)(uintptr_t)0x9999;

    assert(svc_tcp_connect_audio(&opts, "new.example", 1300) == -1);
    assert(opts.audio_in_type == AUDIO_IN_TCP);
    assert(opts.tcp_sockfd == (dsd_socket_t)12);
    assert(opts.tcp_in_ctx == (tcp_input_ctx*)(uintptr_t)0x1234);
    assert(strcmp(opts.audio_in_dev, "tcp:old.example:1200") == 0);
    assert(strcmp(opts.tcp_hostname, "old.example") == 0);
    assert(opts.tcp_portno == 1200);
    assert(g_connect_calls == 1);
    assert(strcmp(g_last_connect_host, "new.example") == 0);
    assert(g_last_connect_port == 1300);
    assert(g_tcp_open_calls == 0);
    assert(g_close_audio_input_calls == 0);
    assert(g_tcp_close_calls == 0);
    assert(g_socket_close_calls == 0);
    assert(g_resampler_reset_calls == 0);
}

static void
test_switches_tcp_backend_only_after_successful_open(void) {
    reset_fakes();

    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 8000;
    opts.tcp_sockfd = (dsd_socket_t)21;
    opts.tcp_in_ctx = (tcp_input_ctx*)(uintptr_t)0x2222;
    opts.input_upsample_prev = 55.0f;
    opts.input_upsample_len = 4;
    opts.input_upsample_pos = 2;
    opts.input_upsample_prev_valid = 1;
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        opts.input_upsample_buf[i] = (float)(i + 1);
    }

    g_connect_result = (dsd_socket_t)88;
    g_tcp_open_result = (tcp_input_ctx*)(uintptr_t)0x3333;

    assert(svc_tcp_connect_audio(&opts, "live.example", 1400) == 0);
    assert(opts.audio_in_type == AUDIO_IN_TCP);
    assert(opts.tcp_sockfd == (dsd_socket_t)88);
    assert(opts.tcp_in_ctx == (tcp_input_ctx*)(uintptr_t)0x3333);
    assert(strcmp(opts.tcp_hostname, "live.example") == 0);
    assert(opts.tcp_portno == 1400);
    assert(g_tcp_close_calls == 1);
    assert(g_last_tcp_close_ctx == (tcp_input_ctx*)(uintptr_t)0x2222);
    assert(g_socket_close_calls == 1);
    assert(g_closed_sockets[0] == (dsd_socket_t)21);
    assert(g_close_audio_input_calls == 0);
    assert(g_resampler_reset_calls == 1);
    assert(opts.input_upsample_prev == 0.0f);
    assert(opts.input_upsample_len == 0);
    assert(opts.input_upsample_pos == 0);
    assert(opts.input_upsample_prev_valid == 0);
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        assert(opts.input_upsample_buf[i] == 0.0f);
    }
}

int
main(void) {
    test_preserves_wav_input_when_tcp_backend_open_fails();
    test_preserves_existing_tcp_input_when_reopen_fails();
    test_preserves_existing_tcp_input_when_reconnect_fails_before_open();
    test_switches_tcp_backend_only_after_successful_open();
    return 0;
}
