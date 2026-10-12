// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/m17_udp.h>
#include <dsd-neo/io/udp_audio.h>
#include <dsd-neo/io/udp_socket_connect.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <errno.h>
#include <netinet/in.h>
#endif
#include <stdint.h>
#include <stdio.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static struct sockaddr_in address;
static struct sockaddr_in addressA;

/* At most one failure line a minute per socket; the failures held back are counted into the next one. */
#define UDP_AUDIO_REPORT_INTERVAL_MS 60000U

/* Send failures of one blaster socket, reported once per run and recovery (issue #659). Decoder thread only. */
typedef struct {
    int failing;          /* the latest send failed */
    int episode_reported; /* the current run of failures was reported, so its end is too */
    uint64_t last_report_ms;
    uint64_t held_back;
} udp_audio_send_report;

static udp_audio_send_report g_udp_report_digital;
static udp_audio_send_report g_udp_report_analog;

static void
udp_audio_note_send(udp_audio_send_report* r, const char* name, int sent, size_t nsam) {
    if (sent >= 0 && (size_t)sent == nsam) {
        if (r->failing && r->episode_reported) {
            LOG_INFO("UDP audio (%s): sending again\n", name);
        }
        r->failing = 0;
        r->episode_reported = 0;
        return;
    }
    const int starts_run = !r->failing;
    r->failing = 1;
    const uint64_t now_ms = dsd_realtime_mono_ms();
    if (!(r->last_report_ms == 0U || now_ms - r->last_report_ms >= UDP_AUDIO_REPORT_INTERVAL_MS)) {
        r->held_back++;
        if (starts_run) {
            r->episode_reported = 0;
        }
        return;
    }
    char held_back[64] = "";
    if (r->held_back > 0U) {
        DSD_SNPRINTF(held_back, sizeof(held_back), " (%llu more failures since the last report)",
                     (unsigned long long)r->held_back);
    }
    if (sent < 0) {
        LOG_WARN("UDP audio (%s): send failed (socket error %d); its audio is dropped while sends fail%s\n", name,
                 dsd_socket_get_error(), held_back);
    } else {
        LOG_WARN("UDP audio (%s): sent %d of %zu bytes%s\n", name, sent, nsam, held_back);
    }
    r->last_report_ms = now_ms == 0U ? 1U : now_ms;
    r->held_back = 0U;
    r->episode_reported = 1;
}

static int
m17_socket_receive_error_is_retryable(void) {
    const int error = dsd_socket_get_error();
#if DSD_PLATFORM_WIN_NATIVE
    return error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
#else
    return error == EINTR || error == EAGAIN || (EWOULDBLOCK != EAGAIN && error == EWOULDBLOCK) || error == ETIMEDOUT;
#endif
}

void
udp_socket_blaster(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    UNUSED(state);
    int err = 0;

    //listen with:

    //short 8k/2
    //socat stdio udp-listen:23456 | play --buffer 640 -q -b 16 -r 8000 -c2 -t s16 -

    //short 8k/1
    //socat stdio udp-listen:23456 | play --buffer 320 -q -b 16 -r 8000 -c1 -t s16 -

    //float 8k/2
    //socat stdio udp-listen:23456 | play --buffer 1280 -q -e float -b 32 -r 8000 -c2 -t f32 -

    //float 8k/1
    //socat stdio udp-listen:23456 | play --buffer 640 -q -e float -b 32 -r 8000 -c1 -t f32 -

    //send audio or data to socket
    err = dsd_socket_sendto(opts->udp_sockfd, data, nsam, 0, (const struct sockaddr*)&address,
                            sizeof(struct sockaddr_in));
    udp_audio_note_send(&g_udp_report_digital, "digital", err, nsam);
}

int
m17_socket_receiver(const dsd_opts* opts, void* data) {
    int err = 0;
    int len = sizeof(address);

    //receive data from socket
    err = dsd_socket_recvfrom(opts->udp_sockfd, data, 1000, 0, (struct sockaddr*)&address,
                              &len); //was MSG_WAITALL, but that seems to be = 256

    if (err < 0 && m17_socket_receive_error_is_retryable()) {
        return 0;
    }

    return err;
}

//Analog UDP port on +2 of normal open socket
void
udp_socket_blasterA(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    UNUSED(state);
    int err = 0;

    //listen with:

    //short 48k/1
    //socat stdio udp-listen:23456 | play --buffer 1920 -q -b 16 -r 48000 -c1 -t s16 -

    if (opts->udp_sockfdA == DSD_INVALID_SOCKET) {
        /* No analog socket (never opened, or opening it failed and was reported then): nothing to send to. */
        return;
    }

    //send audio or data to socket
    err = dsd_socket_sendto(opts->udp_sockfdA, data, nsam, 0, (const struct sockaddr*)&addressA,
                            sizeof(struct sockaddr_in));
    udp_audio_note_send(&g_udp_report_analog, "analog", err, nsam);
}

int
udp_socket_connect(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    int err = 0;
    opts->udp_sockfd = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (opts->udp_sockfd == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, " UDP Socket Error\n");
        return -1;
    }

    // Don't think this is needed, but doesn't seem to hurt to keep it here either
    int broadcastEnable = 1;
    err = dsd_socket_setsockopt(opts->udp_sockfd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    if (err != 0) {
        DSD_FPRINTF(stderr, " UDP Broadcast Set Error %d\n", err);
        dsd_socket_close(opts->udp_sockfd);
        opts->udp_sockfd = DSD_INVALID_SOCKET;
        return err;
    }

    DSD_MEMSET((char*)&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (dsd_socket_resolve(opts->udp_hostname, opts->udp_portno, &address) != 0) {
        DSD_FPRINTF(stderr, " UDP address resolve error for %s\n", opts->udp_hostname);
        dsd_socket_close(opts->udp_sockfd);
        opts->udp_sockfd = DSD_INVALID_SOCKET;
        return -1;
    }

    DSD_MEMSET(&g_udp_report_digital, 0, sizeof(g_udp_report_digital));
    return 0;
}

int
udp_socket_connectA(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    int err = 0;
    opts->udp_sockfdA = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (opts->udp_sockfdA == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, " UDP Socket Error\n");
        return -1;
    }

    // Don't think this is needed, but doesn't seem to hurt to keep it here either
    int broadcastEnable = 1;
    err = dsd_socket_setsockopt(opts->udp_sockfdA, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    if (err != 0) {
        DSD_FPRINTF(stderr, " UDP Broadcast Set Error %d\n", err);
        dsd_socket_close(opts->udp_sockfdA);
        opts->udp_sockfdA = DSD_INVALID_SOCKET;
        return err;
    }

    DSD_MEMSET((char*)&addressA, 0, sizeof(addressA));
    addressA.sin_family = AF_INET;
    //plus 2 to current port assignment for the analog port value
    if (dsd_socket_resolve(opts->udp_hostname, opts->udp_portno + 2, &addressA) != 0) {
        DSD_FPRINTF(stderr, " UDP address resolve error for %s\n", opts->udp_hostname);
        dsd_socket_close(opts->udp_sockfdA);
        opts->udp_sockfdA = DSD_INVALID_SOCKET;
        return -1;
    }

    DSD_MEMSET(&g_udp_report_analog, 0, sizeof(g_udp_report_analog));
    return 0;
}
