// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/ui/menu_services.h>
#include <stdio.h>

int
svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port) {
    if (!opts || !host || port <= 0) {
        return -1;
    }

    char next_host[sizeof opts->tcp_hostname];
    snprintf(next_host, sizeof next_host, "%s", host);

    dsd_socket_t new_sockfd = Connect(next_host, port);
    if (new_sockfd == 0 || new_sockfd == DSD_INVALID_SOCKET) {
        return -1;
    }

    tcp_input_ctx* new_tcp_in_ctx = tcp_input_open(new_sockfd, opts->wav_sample_rate);
    if (new_tcp_in_ctx == NULL) {
        LOG_ERROR("Error, couldn't open TCP audio input\n");
        dsd_socket_close(new_sockfd);
        return -1;
    }

    tcp_input_ctx* old_tcp_in_ctx = opts->tcp_in_ctx;
    dsd_socket_t old_sockfd = opts->tcp_sockfd;
    int old_audio_in_type = opts->audio_in_type;

    opts->tcp_in_ctx = new_tcp_in_ctx;
    opts->tcp_sockfd = new_sockfd;
    snprintf(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", next_host);
    opts->tcp_portno = port;

    if (old_audio_in_type == AUDIO_IN_PULSE) {
        closeAudioInput(opts);
    }
    if (old_tcp_in_ctx) {
        tcp_input_close(old_tcp_in_ctx);
    }
    if (old_sockfd != 0 && old_sockfd != DSD_INVALID_SOCKET && old_sockfd != new_sockfd) {
        dsd_socket_close(old_sockfd);
    }

    dsd_opts_reset_pcm_input_state(opts);
    opts->audio_in_type = AUDIO_IN_TCP;
    return 0;
}
