// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * dsd_rigctl.c
 * Simple RIGCTL Client for DSD (remote control of GQRX, SDR++, etc)
 *
 * Portions from https://github.com/neural75/gqrx-scanner
 *
 * LWVMOBILE
 * 2022-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <limits.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define BUFSIZE        1024
#define FREQ_MAX       4096
#define SAVED_FREQ_MAX 1000
#define TAG_MAX        100

/* Forward declarations for non-static rigctl helpers exported by this TU. */
static bool Send(dsd_socket_t sockfd, const char* buf);
static bool Recv(dsd_socket_t sockfd, char* buf);

/**
 * @brief Establish a TCP RIGCTL connection to the given host/port.
 *
 * Resolves the hostname, opens a TCP socket, and applies a short receive
 * timeout so control I/O cannot wedge the application.
 *
 * @param hostname Target host (IPv4/hostname).
 * @param portno Target port number.
 * @return Socket FD on success; DSD_INVALID_SOCKET on resolution/connection failure.
 */
dsd_socket_t
Connect(char* hostname, int portno) {
    dsd_socket_t sockfd;
    struct sockaddr_in serveraddr;

    /* socket: create the socket */
    sockfd = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sockfd == DSD_INVALID_SOCKET) {
        LOG_ERROR("ERROR opening socket\n");
        perror("ERROR opening socket");
        return DSD_INVALID_SOCKET;
    }

    /* Resolve hostname and build the server's Internet address */
    if (dsd_socket_resolve(hostname, portno, &serveraddr) != 0) {
        LOG_ERROR("ERROR, no such host as %s\n", hostname);
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }

    /* connect: create a connection with the server */
    if (dsd_socket_connect(sockfd, (const struct sockaddr*)&serveraddr, sizeof(serveraddr)) != 0) {
        LOG_ERROR("ERROR connecting socket\n");
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }

    /* Apply small receive timeout so control I/O can't wedge the app. Default 1500ms. */
    {
        int to_ms = 1500;
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (!cfg) {
            dsd_neo_config_init();
            cfg = dsd_neo_get_config();
        }
        if (cfg) {
            to_ms = cfg->rigctl_rcvtimeo_ms;
            if (!cfg->rigctl_rcvtimeo_is_set && cfg->tcp_rcvtimeo_is_set) {
                to_ms = cfg->tcp_rcvtimeo_ms;
            }
        }
        (void)dsd_socket_set_recv_timeout(sockfd, (unsigned int)to_ms);
        int nodelay = 1;
        (void)dsd_socket_setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    return sockfd;
}

/**
 * @brief Send a null-terminated RIGCTL command.
 *
 * Writes the buffer contents and treats short writes as errors.
 *
 * @param sockfd Connected socket FD.
 * @param buf Command buffer (null-terminated).
 * @return true on success; false on error.
 */
static bool
Send(dsd_socket_t sockfd, const char* buf) {
    if (buf == NULL) {
        return false;
    }
    const size_t len = strlen(buf);
    const int n = dsd_socket_send(sockfd, buf, len, 0);
    return n >= 0 && (size_t)n == len;
}

/**
 * @brief Receive a RIGCTL response into the provided buffer.
 *
 * Reads up to BUFSIZE bytes; on timeout/error, zeroes the buffer and returns
 * false.
 *
 * @param sockfd Connected socket FD.
 * @param buf Buffer to fill (must be at least BUFSIZE+1 bytes).
 * @return true on success; false on timeout/error.
 */
static bool
Recv(dsd_socket_t sockfd, char* buf) {
    int n;

    n = dsd_socket_recv(sockfd, buf, BUFSIZE, 0);
    if (n <= 0) {
        // Timeout or error: treat as soft failure so callers can continue
        if (buf) {
            buf[0] = '\0';
        }
        return false;
    }
    buf[n] = '\0';
    return true;
}

/**
 * @brief Query current tuned frequency via RIGCTL.
 *
 * Issues the "f" command and parses the returned frequency in Hz.
 *
 * @param sockfd Connected RIGCTL socket.
 * @return Current frequency in Hz; 0 on error/unknown.
 */
long int
GetCurrentFreq(dsd_socket_t sockfd) {
    long int freq = 0;
    char buf[BUFSIZE];
    char* ptr;
    const char* token;
    char* saveptr = NULL;

    if (!Send(sockfd, "f\n") || !Recv(sockfd, buf)) {
        return 0;
    }

    if (strncmp(buf, "RPRT ", 5) == 0) {
        return freq;
    }

    token = dsd_strtok_r(buf, "\n", &saveptr);
    if (token == NULL) {
        return 0;
    }
    freq = strtol(token, &ptr, 10);
    if (ptr == token) {
        return 0;
    }
    return freq;
}

static bool
rigctl_response_ok(const char* response) {
    if (response == NULL || strncmp(response, "RPRT ", 5) != 0) {
        return false;
    }
    char* end = NULL;
    const long code = strtol(response + 5, &end, 10);
    return end != response + 5 && code == 0;
}

/**
 * @brief Set center frequency on the connected RIGCTL peer.
 *
 * Caches the last request per socket to avoid redundant I/O.
 *
 * @param sockfd Connected RIGCTL socket.
 * @param freq Desired frequency in Hz.
 * @return true on success; false on failure.
 */
bool
SetFreq(dsd_socket_t sockfd, long int freq) {
    static dsd_socket_t s_last_sockfd = DSD_INVALID_SOCKET;
    static long int s_last_freq = LONG_MIN;
    if (sockfd == s_last_sockfd && freq == s_last_freq) {
        return true; // no change; skip I/O
    }
    char buf[BUFSIZE];

    DSD_SNPRINTF(buf, sizeof buf, "F %ld\n", freq);
    if (!Send(sockfd, buf) || !Recv(sockfd, buf) || !rigctl_response_ok(buf)) {
        return false;
    }

    s_last_sockfd = sockfd;
    s_last_freq = freq;
    return true;
}

/**
 * @brief Set modulation/bandwidth on the RIGCTL peer.
 *
 * Sends both the SDR++-specific "NFM" token and the generic "FM" token as a
 * fallback. Requests are cached to skip redundant updates.
 *
 * @param sockfd Connected RIGCTL socket.
 * @param bandwidth Target bandwidth in Hz.
 * @return true on success; false on failure.
 */
bool
SetModulation(dsd_socket_t sockfd, int bandwidth) {
    static dsd_socket_t s_last_sockfd = DSD_INVALID_SOCKET;
    static int s_last_bw = INT_MIN;
    if (sockfd == s_last_sockfd && bandwidth == s_last_bw) {
        return true; // unchanged
    }
    char buf[BUFSIZE];
    /* Active rigctl peers disagree on the narrow-FM token: SDR++ expects NFM,
     * while GQRX and other Hamlib-compatible peers commonly expect FM. */
    DSD_SNPRINTF(buf, sizeof buf, "M NFM %d\n", bandwidth);
    if (!Send(sockfd, buf) || !Recv(sockfd, buf)) {
        return false;
    }

    /* Retry with the token used by the other active peer family. */
    if (!rigctl_response_ok(buf)) {
        DSD_SNPRINTF(buf, sizeof buf, "M FM %d\n", bandwidth);
        if (!Send(sockfd, buf) || !Recv(sockfd, buf)) {
            return false;
        }
    }

    if (!rigctl_response_ok(buf)) {
        return false;
    }

    s_last_sockfd = sockfd;
    s_last_bw = bandwidth;
    return true;
}

static int
set_rigctl_frequency(const dsd_opts* opts, long int freq) {
    if (opts->rigctl_sockfd == DSD_INVALID_SOCKET) {
        return -1;
    }
    if (opts->setmod_bw != 0 && !SetModulation(opts->rigctl_sockfd, opts->setmod_bw)) {
        return -1;
    }
    return SetFreq(opts->rigctl_sockfd, freq) ? 0 : -1;
}

#ifdef USE_RADIO
static int
set_rtl_frequency(dsd_opts* opts, dsd_state* state, uint32_t requested_freq, uint32_t* applied_freq) {
    if (!state || !state->rtl_ctx) {
        return -1;
    }

    int tune_result = rtl_stream_tune(state->rtl_ctx, requested_freq);
    if (tune_result != RTL_STREAM_TUNE_OK) {
        if (tune_result == RTL_STREAM_TUNE_TIMEOUT) {
            /* The controller accepted the request and may complete after the
             * synchronous wait expires. */
            opts->rtlsdr_center_freq = requested_freq;
        }
        return tune_result;
    }

    /* Controller requests can coalesce, so cache the target that actually
     * completed rather than this caller's requested one. */
    uint32_t controller_freq = 0U;
    if (rtl_stream_get_last_applied_freq(&controller_freq) == 0 && controller_freq != 0U) {
        *applied_freq = controller_freq;
    }
    return 0;
}
#endif

/**
 * @brief Set tuner frequency via io/control API (simple tune without trunking bookkeeping).
 *
 * This is the canonical way for UI and non-trunking code to request frequency changes.
 * It handles both RTL-SDR and rigctl backends but does NOT update trunking state fields
 * or perform modulation resets. For trunking voice/CC tuning, use
 * dsd_trunk_tuning_hook_tune_to_freq() or dsd_trunk_tuning_hook_tune_to_cc() instead.
 *
 * @param opts Decoder options with tuning configuration.
 * @param state Decoder state (required for RTL tuning, may be NULL for rigctl-only).
 * @param freq Target frequency in Hz.
 * @return 0 on success, 1 when an RTL tune is deferred, or a negative error/timeout code. An RTL timeout leaves an
 *         accepted request active and retains its target in opts->rtlsdr_center_freq.
 */
int
io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || freq <= 0) {
        return -1;
    }
    uint32_t applied_freq = (uint32_t)freq;
#ifndef USE_RADIO
    (void)state;
#endif

    LOG_INFO("io_control: tune to %ld Hz\n", freq);

    int rc = -1;
    if (opts->use_rigctl == 1) {
        rc = set_rigctl_frequency(opts, freq);
#ifdef USE_RADIO
    } else if (opts->audio_in_type == AUDIO_IN_RTL) {
        rc = set_rtl_frequency(opts, state, applied_freq, &applied_freq);
#endif
    }
    if (rc != 0) {
        return rc;
    }
    // Update cached frequency only after the selected backend accepts the request.
    opts->rtlsdr_center_freq = applied_freq;
    return 0;
}
