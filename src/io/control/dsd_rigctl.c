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
#include <dsd-neo/runtime/analog_channel.h>
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
static void rigctl_peer_forget(dsd_socket_t sockfd);

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

    /* A new connection on the number of a socket closed before it is a peer nothing was asked of. */
    rigctl_peer_forget(sockfd);
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
    char buf[BUFSIZE + 1]; /* Recv() terminates a full BUFSIZE-byte reply */
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

/* The frequency SetFreq() last had accepted, and the socket it went out on (RigctlRebindPeer() forgets it). */
static dsd_socket_t s_last_sockfd = DSD_INVALID_SOCKET;
static long int s_last_freq = LONG_MIN;

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
    if (sockfd == s_last_sockfd && freq == s_last_freq) {
        return true; // no change; skip I/O
    }
    char buf[BUFSIZE + 1]; /* Recv() terminates a full BUFSIZE-byte reply */

    DSD_SNPRINTF(buf, sizeof buf, "F %ld\n", freq);
    if (!Send(sockfd, buf) || !Recv(sockfd, buf) || !rigctl_response_ok(buf)) {
        return false;
    }

    s_last_sockfd = sockfd;
    s_last_freq = freq;
    return true;
}

/* What this client knows of the rigctl peer on one socket (issue #526).
 *
 * The cache is the demodulator and passband the peer last accepted, keyed together so an FM and an AM request of the
 * same width are two requests; a passband of 0 is the peer's own. A lost reply leaves the passband not known (INT_MIN),
 * and the demodulator too (DSD_RIGCTL_KIND_UNKNOWN) when the request asked for the other one. SDR++ and GQRX take a
 * passband of 0 as "leave it unchanged", not as their normal one, and SDR++ keeps every passband it is sent across
 * restarts, so a scan row's own passband stays in force until it is sent back. The peer's own passband of each
 * demodulator is therefore read ("m") before a scan row first changes it, and it is what the peer is asked for to undo
 * that. */
typedef struct {
    dsd_socket_t sockfd;
    int touched;        /* a request this client made on the socket may have changed the peer */
    int unconfirmed;    /* a reconnect took the record over: no request matches the cache until the peer accepts one */
    int kind;           /* cache: the demodulator last accepted (DSD_RIGCTL_KIND_UNKNOWN: not known) */
    int bw;             /* cache: the passband last accepted (0: the peer's own; INT_MIN: not known) */
    int own_read[2];    /* per dsd_analog_demod: the peer's own passband was looked up */
    int own_bw[2];      /* ...and is this many Hz (0: not known) */
    int row_changed[2]; /* a scan row asked for a passband of this demodulator since the scan's last restore */
} rigctl_peer;

static rigctl_peer s_peer = {DSD_INVALID_SOCKET, 0, 0, DSD_ANALOG_DEMOD_FM, INT_MIN, {0, 0}, {0, 0}, {0, 0}};

static void
rigctl_peer_reset(dsd_socket_t sockfd) {
    DSD_MEMSET(&s_peer, 0, sizeof s_peer);
    s_peer.sockfd = sockfd;
    s_peer.kind = DSD_ANALOG_DEMOD_FM;
    s_peer.bw = INT_MIN;
}

/* Forget the record when a new connection is opened on its socket's number: that socket was closed. A connection on
 * another number (the TCP audio input's reconnect, say) leaves the record of the still-open rigctl socket alone. */
static void
rigctl_peer_forget(dsd_socket_t sockfd) {
    if (s_peer.sockfd == sockfd) {
        rigctl_peer_reset(DSD_INVALID_SOCKET);
    }
}

/* The record for @p sockfd: a socket the record does not describe is a peer nothing was asked of yet. */
static rigctl_peer*
rigctl_peer_on(dsd_socket_t sockfd) {
    if (s_peer.sockfd != sockfd) {
        rigctl_peer_reset(sockfd);
    }
    return &s_peer;
}

void
RigctlRebindPeer(dsd_socket_t old_fd, dsd_socket_t new_fd, int same_endpoint) {
    /* The frequency last sent went out on the connection being replaced, whose socket is closed next: a later
     * connection that gets its number back would otherwise be taken for one on that frequency. */
    s_last_sockfd = DSD_INVALID_SOCKET;
    s_last_freq = LONG_MIN;
    /* A reset record's socket is DSD_INVALID_SOCKET, which a missing old socket must not match. Only a record of a
     * request that may have changed the old socket's peer is worth handing over. */
    if (old_fd != DSD_INVALID_SOCKET && s_peer.sockfd == old_fd && s_peer.touched) {
        if (same_endpoint) {
            /* The same peer: what the scan changed on it, its own passbands, and what it last accepted (which a failed
             * tune puts back) stay known. It may have restarted, or been changed, while the connection was down
             * (often why it is reconnected), so no request is taken for one it already runs until it accepts one. */
            s_peer.sockfd = new_fd;
            s_peer.unconfirmed = 1;
            return;
        }
        if (s_peer.kind != DSD_ANALOG_DEMOD_FM) {
            /* Another endpoint may be another peer, whose own passbands the record does not describe, or the same one
             * under another name, still on the am row's AM (or on either demodulator after a lost reply): FM is sent
             * before anything else is taken for it. */
            rigctl_peer_reset(new_fd);
            s_peer.touched = 1;
            s_peer.kind = DSD_RIGCTL_KIND_UNKNOWN;
            return;
        }
    }
    /* Otherwise the new peer is one nothing was asked of, as on a first connection, and keeps its own settings. The
     * record names it now, not the old socket closed next: a connection that gets that number back (the TCP audio
     * input's, outside the P25 SM tick guard) would otherwise reset the record the watchdog's retunes use. */
    rigctl_peer_reset(new_fd);
}

#ifdef DSD_NEO_TEST_HOOKS
dsd_socket_t
dsd_rigctl_test_record_socket(void) {
    return s_peer.sockfd;
}
#endif

static int
rigctl_kind(int kind) {
    return kind == DSD_ANALOG_DEMOD_AM ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
}

/* Send "M <token> <bandwidth>" and read the reply into @p buf. Returns 1 when the peer accepted it, 0 when it refused,
 * -1 when the I/O failed. */
static int
rigctl_set_mode(dsd_socket_t sockfd, const char* token, int bandwidth, char* buf) {
    DSD_SNPRINTF(buf, BUFSIZE, "M %s %d\n", token, bandwidth);
    if (!Send(sockfd, buf) || !Recv(sockfd, buf)) {
        return -1;
    }
    return rigctl_response_ok(buf) ? 1 : 0;
}

/* Ask the peer for demodulator @p kind at passband @p send_bw and record the answer, with @p cache_bw as the passband
 * the cache keeps (0 when @p send_bw is the peer's own). Returns what rigctl_set_mode() does. */
static int
rigctl_request(rigctl_peer* peer, int kind, int send_bw, int cache_bw, char* buf) {
    int rc;
    if (kind == DSD_ANALOG_DEMOD_AM) {
        rc = rigctl_set_mode(peer->sockfd, "AM", send_bw, buf);
    } else {
        /* Active rigctl peers disagree on the narrow-FM token: some expect NFM, while GQRX, SDR++ and other
         * Hamlib-compatible peers expect FM. */
        rc = rigctl_set_mode(peer->sockfd, "NFM", send_bw, buf);
        if (rc == 0) {
            /* Retry with the token used by the other active peer family. */
            rc = rigctl_set_mode(peer->sockfd, "FM", send_bw, buf);
        }
    }
    if (rc == -1) {
        /* The request may have reached the peer before the reply was lost, so what it runs is no longer known: no
         * request matches the cache until one is answered, the FM undo at the peer's own passband included. A request
         * for the other demodulator leaves the peer on either one, so a best-effort tune that reads it
         * (CachedModulationKind()) fails until the peer accepts one. */
        peer->touched = 1;
        if (peer->kind != kind) {
            peer->kind = DSD_RIGCTL_KIND_UNKNOWN;
        }
        peer->bw = INT_MIN;
    } else if (rc == 1) {
        peer->touched = 1;
        peer->unconfirmed = 0;
        peer->kind = kind;
        peer->bw = cache_bw;
    }
    return rc;
}

/* Read the rest of a reply into @p buf (holding @p len bytes) until it has @p lines newline-terminated lines. */
static int
rigctl_recv_lines(dsd_socket_t sockfd, char* buf, size_t len, int lines) {
    for (;;) {
        int seen = 0;
        for (const char* p = buf; (p = strchr(p, '\n')) != NULL; p++) {
            seen++;
        }
        if (seen >= lines || len >= BUFSIZE) {
            return seen >= lines;
        }
        const int n = dsd_socket_recv(sockfd, buf + len, BUFSIZE - len, 0);
        if (n <= 0) {
            return 0;
        }
        len += (size_t)n;
        buf[len] = '\0';
    }
}

/* The widest passband a peer's "m" reply is taken at (a RAW or WFM passband is far below it). */
static const long k_rigctl_passband_max_hz = 100000000L;

/* Ask the peer which demodulator and passband it runs ("m", answered "<MODE>\n<passband>\n" by SDR++, GQRX and
 * Hamlib). Returns 1 with @p kind (dsd_analog_demod, or -1 for a mode that is neither narrow FM nor AM) and
 * @p passband_hz (0 when the peer names none, or none a receiver passband can be), 0 when the peer answered something
 * else, -1 when the I/O failed. The whole first line is read before it is told apart: a refusal ("RPRT -11") can
 * arrive over several reads, and a part of it left unread would be taken for the reply to the next command. */
static int
rigctl_get_mode(dsd_socket_t sockfd, char* buf, int* kind, int* passband_hz) {
    if (!Send(sockfd, "m\n") || !Recv(sockfd, buf) || !rigctl_recv_lines(sockfd, buf, strlen(buf), 1)) {
        return -1;
    }
    if (strncmp(buf, "RPRT", 4) == 0) {
        return 0;
    }
    if (!rigctl_recv_lines(sockfd, buf, strlen(buf), 2)) {
        return -1;
    }
    char* mode = buf;
    char* passband = strchr(buf, '\n');
    if (passband == NULL) {
        return 0; /* rigctl_recv_lines() found two lines; nothing to parse otherwise */
    }
    *passband++ = '\0';
    mode[strcspn(mode, "\r")] = '\0';
    char* end = NULL;
    const long hz = strtol(passband, &end, 10);
    if (end == passband || (*end != '\n' && *end != '\r')) {
        return 0;
    }
    *kind = -1;
    if (strcmp(mode, "FM") == 0 || strcmp(mode, "NFM") == 0) {
        *kind = DSD_ANALOG_DEMOD_FM;
    } else if (strcmp(mode, "AM") == 0) {
        *kind = DSD_ANALOG_DEMOD_AM;
    }
    *passband_hz = (hz > 0 && hz <= k_rigctl_passband_max_hz) ? (int)hz : 0;
    return 1;
}

/* Look up, once per socket and scan, the peer's own passband of demodulator @p kind before a scan row first changes
 * it. A peer running another demodulator is switched to this one at passband 0, which keeps its own, and asked again;
 * the passband of the one it ran is kept too, unless a row already changed it. A peer that cannot say leaves it not
 * known. */
static void
rigctl_read_own_passband(rigctl_peer* peer, int kind, char* buf) {
    if (peer->own_read[kind]) {
        return;
    }
    peer->own_read[kind] = 1;
    int now_kind = -1;
    int passband = 0;
    if (rigctl_get_mode(peer->sockfd, buf, &now_kind, &passband) != 1) {
        return;
    }
    if (now_kind == kind) {
        peer->own_bw[kind] = passband;
        return;
    }
    if (now_kind >= 0 && !peer->own_read[now_kind]) {
        peer->own_read[now_kind] = 1;
        peer->own_bw[now_kind] = passband;
    }
    if (rigctl_request(peer, kind, 0, 0, buf) == 1 && rigctl_get_mode(peer->sockfd, buf, &now_kind, &passband) == 1
        && now_kind == kind) {
        peer->own_bw[kind] = passband;
    }
}

/**
 * @brief Set modulation/bandwidth on the RIGCTL peer.
 *
 * Sends the "NFM" token, then the generic "FM" token (SDR++, GQRX and Hamlib)
 * when the peer refuses it. Requests are cached to skip redundant updates.
 *
 * @param sockfd Connected RIGCTL socket.
 * @param bandwidth Target bandwidth in Hz.
 * @return true on success; false on failure.
 */
bool
SetModulation(dsd_socket_t sockfd, int bandwidth) {
    return SetModulationKind(sockfd, DSD_ANALOG_DEMOD_FM, bandwidth);
}

bool
SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth) {
    rigctl_peer* peer = rigctl_peer_on(sockfd);
    const int want_kind = rigctl_kind(kind);
    int send_bw = bandwidth;
    if (want_kind == DSD_ANALOG_DEMOD_FM && bandwidth == 0) {
        /* FM at the peer's own passband only undoes a request this client made on the socket (an AM row's
         * demodulator, or a row's own passband): a peer nothing was asked of keeps its own settings. */
        if (!peer->touched) {
            return true;
        }
        /* The passband read before a row changed it; where the peer could not say, Hamlib's 0 (normal passband). */
        send_bw = peer->own_bw[DSD_ANALOG_DEMOD_FM];
    }
    if (peer->kind == want_kind && peer->bw == bandwidth && !peer->unconfirmed) {
        return true; // unchanged
    }
    char buf[BUFSIZE + 1];
    return rigctl_request(peer, want_kind, send_bw, bandwidth, buf) == 1;
}

int
CachedModulationKind(dsd_socket_t sockfd) {
    /* A socket the record does not describe is a peer nothing was asked of (rigctl_peer_on()). */
    return s_peer.sockfd == sockfd ? s_peer.kind : DSD_ANALOG_DEMOD_FM;
}

dsd_rigctl_modulation
CachedModulation(dsd_socket_t sockfd) {
    /* A socket the record does not describe is a peer nothing was asked of (rigctl_peer_reset()). */
    dsd_rigctl_modulation now = {DSD_ANALOG_DEMOD_FM, INT_MIN};
    if (s_peer.sockfd == sockfd) {
        now.kind = s_peer.kind;
        now.bandwidth = s_peer.bw;
    }
    return now;
}

bool
RevertModulation(dsd_socket_t sockfd, dsd_rigctl_modulation before) {
    rigctl_peer* peer = rigctl_peer_on(sockfd);
    if (peer->kind == before.kind && peer->bw == before.bandwidth) {
        return true; /* the tune changed nothing this client knew of */
    }
    if (before.kind != DSD_ANALOG_DEMOD_FM && before.kind != DSD_ANALOG_DEMOD_AM) {
        return false; /* no demodulator known to go back to */
    }
    /* The peer's own passband of that demodulator, read before a row changed it (0: not read). */
    const int own_bw = peer->own_bw[before.kind];
    if (before.bandwidth == INT_MIN && peer->kind == before.kind && (peer->bw == 0 || own_bw <= 0)) {
        /* Its demodulator at a passband not known before the tune (a peer nothing was asked of, or a lost reply): the
         * peer runs its own passband again, or none was read to go back to. */
        return true;
    }
    /* @p before's passband, or for 0 or one not known the peer's own (else 0, Hamlib's normal passband). The peer's own
     * as read is what the cache then holds (0); a 0 sent for one not known leaves it not known. */
    const int send_bw = before.bandwidth > 0 ? before.bandwidth : own_bw;
    const int cache_bw = (before.bandwidth == INT_MIN && own_bw > 0) ? 0 : before.bandwidth;
    char buf[BUFSIZE + 1];
    return rigctl_request(peer, before.kind, send_bw, cache_bw, buf) == 1;
}

bool
SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    rigctl_peer* peer = rigctl_peer_on(sockfd);
    const int want_kind = rigctl_kind(kind);
    if (bandwidth <= 0 || (peer->kind == want_kind && peer->bw == bandwidth)) {
        return SetModulationKind(sockfd, want_kind, bandwidth);
    }
    char buf[BUFSIZE + 1];
    rigctl_read_own_passband(peer, want_kind, buf);
    const int rc = rigctl_request(peer, want_kind, bandwidth, bandwidth, buf);
    if (rc != 0) {
        peer->row_changed[want_kind] = 1;
    }
    return rc == 1;
}

bool
RestoreScanModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    rigctl_peer* peer = rigctl_peer_on(sockfd);
    const int want_kind = rigctl_kind(kind);
    char buf[BUFSIZE + 1];
    int restored[2] = {1, 1};
    /* The other demodulator's passband first, so that the request for what the session runs comes last and leaves the
     * peer on it; that request puts back its own passband where it asks for FM without -B. */
    for (int other = DSD_ANALOG_DEMOD_FM; other <= DSD_ANALOG_DEMOD_AM; other++) {
        if (other != want_kind && peer->row_changed[other] && peer->own_bw[other] > 0) {
            restored[other] = rigctl_request(peer, other, peer->own_bw[other], 0, buf) == 1;
        }
    }
    const bool ok = SetModulationKind(sockfd, want_kind, bandwidth);
    restored[want_kind] = ok ? 1 : 0;
    /* The next scan reads the peer's own passbands again: the operator may change them between scans. A demodulator
     * whose undo the peer did not accept (refused, or its reply lost) may still run a row's passband, which a read
     * would take for the peer's own, so what was read of it stays for a later undo to send. */
    for (int k = DSD_ANALOG_DEMOD_FM; k <= DSD_ANALOG_DEMOD_AM; k++) {
        if (restored[k]) {
            peer->own_read[k] = 0;
            peer->own_bw[k] = 0;
            peer->row_changed[k] = 0;
        }
    }
    return ok;
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
