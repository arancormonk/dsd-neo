// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for optional TCP/UDP PCM input backends.
 *
 * Lower layers should not depend on IO backend headers directly. The engine
 * installs real hook functions at startup; the runtime provides safe wrappers
 * with defaults when hooks are not installed.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_NET_AUDIO_INPUT_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_NET_AUDIO_INPUT_HOOKS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/sockets.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    tcp_input_ctx* (*tcp_open)(dsd_socket_t sockfd, int samplerate);
    void (*tcp_close)(tcp_input_ctx* ctx);
    /* A TCP read that gives up after timeout_ms with no sample (-1) and keeps the connection, as the UDP wait does
       (issue #667 D12): a quiet sender is no closed connection, for the symbol path, EDACS and the M17 encoder. */
    int (*tcp_read_sample_wait)(tcp_input_ctx* ctx, int16_t* out, unsigned int timeout_ms);

    int (*udp_start)(dsd_opts* opts, const char* bindaddr, int port, int samplerate);
    void (*udp_stop)(dsd_opts* opts);
    int (*udp_read_sample)(dsd_opts* opts, int16_t* out);

    /* A UDP read that gives up after timeout_ms with no sample (-1), so the decoder can apply a queued command while
       the input is silent (issue #634). */
    int (*udp_read_sample_wait)(dsd_opts* opts, int16_t* out, unsigned int timeout_ms);
    /* Drop the samples the input held at a rigctl retune's mark (issue #834): the peer's audio from before it. */
    long (*tcp_discard_through)(tcp_input_ctx* ctx, const dsd_input_cut* cut);
    long (*udp_discard_through)(dsd_opts* opts, const dsd_input_cut* cut);
    /* A bounded, cancellable TCP connect (ConnectBounded()), for the TCP audio reconnect. */
    dsd_socket_t (*tcp_connect)(const char* host, int port, int resolve, dsd_socket_cancel_fn cancelled, void* context);
} dsd_net_audio_input_hooks;

void dsd_net_audio_input_hooks_set(dsd_net_audio_input_hooks hooks);

tcp_input_ctx* dsd_net_audio_input_hook_tcp_open(dsd_socket_t sockfd, int samplerate);
void dsd_net_audio_input_hook_tcp_close(tcp_input_ctx* ctx);
/** 1 with a sample, 0 when the connection ended (also without the hook), -1 when none arrived within @p timeout_ms. */
int dsd_net_audio_input_hook_tcp_read_sample_wait(tcp_input_ctx* ctx, int16_t* out, unsigned int timeout_ms);

int dsd_net_audio_input_hook_udp_start(dsd_opts* opts, const char* bindaddr, int port, int samplerate);
void dsd_net_audio_input_hook_udp_stop(dsd_opts* opts);
int dsd_net_audio_input_hook_udp_read_sample(dsd_opts* opts, int16_t* out);
/** 1 with a sample, 0 on stop, -1 when none arrived within @p timeout_ms. Without the hook it waits as
    dsd_net_audio_input_hook_udp_read_sample() does, and never returns -1. */
int dsd_net_audio_input_hook_udp_read_sample_wait(dsd_opts* opts, int16_t* out, unsigned int timeout_ms);
/** The samples the input dropped (tcp_input_discard_through(), udp_input_discard_through()); 0 without the hook. */
long dsd_net_audio_input_hook_tcp_discard_through(tcp_input_ctx* ctx, const dsd_input_cut* cut);
long dsd_net_audio_input_hook_udp_discard_through(dsd_opts* opts, const dsd_input_cut* cut);
/** The connected socket, or DSD_INVALID_SOCKET (also without the hook). */
dsd_socket_t dsd_net_audio_input_hook_tcp_connect(const char* host, int port, int resolve,
                                                  dsd_socket_cancel_fn cancelled, void* context);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_NET_AUDIO_INPUT_HOOKS_H_ */
