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

#include <dsd-neo/core/channel_mode.h>
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
#include <dsd-neo/runtime/rigctl_passband.h>
#include <errno.h>
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
static void connect_apply_options(dsd_socket_t sockfd);
static void connect_note_address(const char* host, int port, const struct sockaddr_in* addr);

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

    connect_apply_options(sockfd);
    connect_note_address(hostname, portno, &serveraddr);
    return sockfd;
}

/* The receive timeout and options every connection Connect() and ConnectBounded() make runs with. */
static void
connect_apply_options(dsd_socket_t sockfd) {
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
}

/* An address a connection was made to (issue #634). */
typedef struct {
    int valid;
    char host[256];
    int port;
    struct sockaddr_in addr;
} connect_address;

/* The address the last connection Connect() or ConnectBounded() made went to, and the one the running TCP audio input's
   connection went to (ConnectKeepTcpAudioAddress()), which its reconnect goes back to rather than looking the host up
   while the decoder waits on it (issue #634). A connection nobody keeps (a switch that failed, a rigctl connection)
   never displaces it. Decoder thread only. */
static connect_address g_connect_last;
static connect_address g_tcp_audio_address;

static void
connect_note_address(const char* host, int port, const struct sockaddr_in* addr) {
    g_connect_last.valid = 0;
    if (strlen(host) >= sizeof g_connect_last.host) {
        return;
    }
    DSD_SNPRINTF(g_connect_last.host, sizeof g_connect_last.host, "%s", host);
    g_connect_last.port = port;
    g_connect_last.addr = *addr;
    g_connect_last.valid = 1;
}

static int
connect_address_is(const connect_address* address, const char* host, int port) {
    return address->valid && address->port == port && strncmp(address->host, host, sizeof address->host) == 0;
}

void
ConnectKeepTcpAudioAddress(const char* hostname, int portno) {
    if (hostname && connect_address_is(&g_connect_last, hostname, portno)) {
        g_tcp_audio_address = g_connect_last;
    }
}

static int
connect_bounded_resolve(const char* hostname, int portno, int resolve, struct sockaddr_in* addr) {
    if (!resolve && connect_address_is(&g_tcp_audio_address, hostname, portno)) {
        *addr = g_tcp_audio_address.addr;
        return 0;
    }
    if (dsd_socket_resolve(hostname, portno, addr) != 0) {
        LOG_ERROR("ERROR, no such host as %s\n", hostname);
        return -1;
    }
    return 0;
}

dsd_socket_t
ConnectBounded(const char* hostname, int portno, int resolve, dsd_socket_cancel_fn cancelled, void* context) {
    if (!hostname || hostname[0] == '\0' || portno <= 0) {
        return DSD_INVALID_SOCKET;
    }
    struct sockaddr_in serveraddr;
    DSD_MEMSET(&serveraddr, 0, sizeof serveraddr);
    if (connect_bounded_resolve(hostname, portno, resolve, &serveraddr) != 0) {
        return DSD_INVALID_SOCKET;
    }
    dsd_socket_t sockfd = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sockfd == DSD_INVALID_SOCKET) {
        LOG_ERROR("ERROR opening socket\n");
        return DSD_INVALID_SOCKET;
    }
    int code = 0;
    if (dsd_socket_connect_bounded(sockfd, (const struct sockaddr*)&serveraddr, sizeof(serveraddr),
                                   DSD_CONNECT_BOUNDED_TIMEOUT_MS, cancelled, context, &code)
        != 0) {
        LOG_ERROR("ERROR connecting socket to %s:%d\n", hostname, portno);
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }
    connect_apply_options(sockfd);
    connect_note_address(hostname, portno, &serveraddr);
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
    /* strtol() clamps an overflow to LONG_MAX, which on Windows (32-bit long) is itself a frequency; a reading that
       does not fit is unknown, not 2147483647 Hz. */
    errno = 0;
    freq = strtol(token, &ptr, 10);
    if (ptr == token || errno != 0 || freq < 0) {
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

/* Who a reading of the peer's own passband serves (rigctl_demod::reading, see rigctl_peer). */
enum rigctl_reading { RIGCTL_READING_NONE = 0, RIGCTL_READING_SCAN, RIGCTL_READING_SESSION };

/* What this client knows of one of the peer's two demodulators (dsd_analog_demod), see rigctl_peer. */
typedef struct {
    int own_hz;     /* the peer's own passband of it, as last read (0: not known) */
    int reading;    /* rigctl_reading: who that reading serves */
    int changed;    /* a passband this client asked for stands on it, as far as this client knows */
    int retired_hz; /* a reading a return retired, held until the next request or revert (0: none) */
} rigctl_demod;

/* What this client knows of the rigctl peer on one socket (issues #526, #589, #621).
 *
 * The peer runs one demodulator at a time, FM or AM. SDR++ keeps a passband per demodulator and saves every passband
 * it is sent across restarts, and SDR++ and GQRX take a passband of 0 as "leave it unchanged": a passband this client
 * sets stays in force until this client sends the peer's own back. So the peer's own passband of a demodulator is read
 * ("m") before this client first changes it, and that reading is what a return sends.
 *
 * The cache (kind, bw) is what the peer last accepted, the demodulator and passband together, so FM and AM at one
 * width are two requests; a passband of 0 is the peer's own. A lost reply leaves the passband not known (INT_MIN), and
 * the demodulator too (DSD_RIGCTL_KIND_UNKNOWN) when the request asked for the other one. After a reconnect to the same
 * endpoint the cache is unconfirmed: the peer may have restarted, so a passband is sent again even where it matches,
 * though a return finds nothing to undo (SetModulationKind()).
 *
 * Two facts of the cache are read in one place each. The peer was left on demodulator K at its own passband
 * (rigctl_peer_left_on_own()): nothing was ever taken, or the last request it accepted was K at 0; a reconnect since
 * changes nothing here, since a restart does not put this client's passband back. The peer is known to run K's own
 * passband as read (rigctl_runs_own_passband()): left on it, with a reading behind the 0; a 0 sent with no reading may
 * have left this client's passband in force, since SDR++ and GQRX keep the passband they had, and a new read would
 * take that passband for the peer's own.
 *
 * Per demodulator (rigctl_demod) the record keeps four things.
 * - own_hz: the peer's own passband of it as last read; 0 when not known (never read, a peer that could not say, or a
 *   reading retired).
 * - reading: who that reading serves. NONE: there is none, and the next passband this client sets on the demodulator
 *   reads first. SCAN: a scan row's lookup took it, of the demodulator asked for or of the other one the peer ran
 *   then; it serves every row of the scan without a new read (issue #526), is what the FM undo and the leave restore
 *   send back, and the restore resets it, except where that demodulator's undo was refused or lost (the peer may still
 *   run this client's passband, which a read would take for its own). SESSION: a request for the session's own
 *   passband took it off any scan (RigctlMarkSessionPassband()), or the leave restore handed it to the session
 *   (RestoreScanModulation() with session_passband); no restore follows, so it lasts the session's need of it and is
 *   retired once the peer is known to run that demodulator's own passband (it accepted the reading back, or refused
 *   the session's request while on its own); the other demodulator's SESSION reading that no change of this client's
 *   holds goes with it, as the same lookup took it. A retired reading is never sent again as a return: the operator
 *   may change a passband the peer runs, so the next change reads afresh. A scan row's taken request makes a SESSION
 *   reading the scan's.
 * - changed: a passband (> 0) this client asked for on the demodulator was taken, or its reply lost, and the peer has
 *   not since accepted that demodulator at its own passband as read. The restore sends own_hz back for a changed
 *   demodulator the session does not run, and a reading an undo still has to send is held.
 * - retired_hz: the reading a return retired (below), held for a failed tune's revert that undoes that return. It lives
 *   from that return until the next request this client sends (for either demodulator, whatever the answer), the next
 *   revert, or a reset of the demodulator's record (rigctl_demod_reset(): a leave restore that puts the demodulator
 *   back, one that sends nothing included, and a fresh record), whichever comes first; in a failed tune that is the
 *   revert, which reads it before its own request. A reading a mark retires is not held.
 * A demodulator with no reading has own_hz 0. Whether it is changed does not depend on a reading: a passband set with
 * none (FOLLOW at -B on an input the peer does not demodulate, say) stands too, with nothing read to put back.
 *
 * Transitions, for the demodulator K a request names:
 * - A capturing request (SetScanRowModulation()) with no reading of K reads first (rigctl_read_own_passband()): K's
 *   reading becomes SCAN with own_hz from "m", and the other demodulator's too when the peer ran it and had none (the
 *   peer is switched to K at passband 0, which keeps K's own, to read that).
 * - Any request this client sends (rigctl_request()), whatever the answer, first drops what a return held for a
 *   revert, of both demodulators; a return sets the holds anew (below). So the holds are what the last request
 *   retired, and a revert can bring back only what the request it undoes retired.
 * - A request for K at a passband > 0 taken, or its reply lost: K is changed. A capturing one
 *   (SetScanRowModulation()) makes a SESSION reading of K the scan's, which a session request marks again right after.
 *   Refused: nothing.
 * - A request for K at its own passband accepted (the cache holds 0), sent as the reading (own_hz > 0): K is no
 *   longer changed, and a SESSION reading of K is retired with the other demodulator's unchanged SESSION reading
 *   (rigctl_retire_session_reading()), each held in retired_hz. Sent as 0, with no reading, it ends nothing: the peer
 *   may keep this client's.
 * - A mark (rigctl_mark_session(): RigctlMarkSessionPassband(), the restore with session_passband): a SCAN reading
 *   of K becomes SESSION, and so does the other demodulator's unchanged SCAN reading; where the peer is known to run
 *   K's own passband (rigctl_runs_own_passband()) the reading is retired at once, with nothing held: a mark follows a
 *   request for a passband > 0, never a return, so the peer runs its own because it did not take that request, and
 *   no revert undoes a return there.
 * - A tune that fails puts back what the peer ran before it (RevertModulation()), which takes the holds whichever way
 *   it goes. Where the tune's last request returned a demodulator to its own passband, they are what that return
 *   retired, and a revert that puts back a passband this client set undoes the return: each comes back for the
 *   session, the other demodulator's included; a revert whose reply was lost may have stood and reinstates too, one
 *   the peer refused leaves the return in force. Any other request the tune made dropped them, so nothing comes back.
 * - The leave restore (RestoreScanModulation()): the other demodulator, changed with a reading, goes back to its own
 *   passband; every demodulator whose undo the peer accepted, or that was never changed, is reset to NONE; the
 *   session's demodulator is kept, and marked where the session's request sets a passband.
 * - A reconnect to the same endpoint keeps everything and leaves the cache unconfirmed; another endpoint starts a
 *   fresh record (RigctlRebindPeer()).
 * - FM at 0 (SetModulationKind()) sends nothing where the peer was left on FM at its own passband
 *   (rigctl_peer_left_on_own()): there is nothing of this client's to undo. Otherwise it sends FM's own_hz (0 where
 *   none), which undoes a passband this client set on FM, or the switch to AM it made. */
typedef struct {
    dsd_socket_t sockfd;
    int touched;     /* a request this client made on the socket may have changed the peer */
    int unconfirmed; /* a reconnect took the record over: no request matches the cache until the peer accepts one */
    int kind;        /* cache: the demodulator last accepted (DSD_RIGCTL_KIND_UNKNOWN: not known) */
    int bw;          /* cache: the passband last accepted (0: the peer's own; INT_MIN: not known) */
    rigctl_demod demod[2]; /* per dsd_analog_demod */
} rigctl_peer;

static rigctl_peer s_peer = {DSD_INVALID_SOCKET, 0, 0, DSD_ANALOG_DEMOD_FM, INT_MIN, {{0, 0, 0, 0}, {0, 0, 0, 0}}};

static void
rigctl_peer_reset(dsd_socket_t sockfd) {
    DSD_MEMSET(&s_peer, 0, sizeof s_peer);
    s_peer.sockfd = sockfd;
    s_peer.kind = DSD_ANALOG_DEMOD_FM;
    s_peer.bw = INT_MIN;
}

/* No reading of the demodulator: the next passband this client sets on it reads first. */
static void
rigctl_demod_reset(rigctl_demod* demod) {
    demod->own_hz = 0;
    demod->reading = RIGCTL_READING_NONE;
    demod->changed = 0;
    demod->retired_hz = 0;
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
            /* The readings and whom they serve stay known too: a return finds nothing to undo where the peer last
             * accepted FM at its own passband, and a reading retired is never sent, so handing them over loses
             * nothing. */
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

static int
rigctl_other_kind(int kind) {
    return kind == DSD_ANALOG_DEMOD_AM ? DSD_ANALOG_DEMOD_FM : DSD_ANALOG_DEMOD_AM;
}

/* Whether the peer was left on demodulator @p kind at its own passband: nothing this client asked of it was ever taken,
 * or the last request it accepted was that demodulator at 0. Nothing of this client's stands on it to undo. A
 * reconnect since (the cache unconfirmed) changes nothing here: a restarted peer does not put this client's passband
 * back. */
static int
rigctl_peer_left_on_own(const rigctl_peer* peer, int kind) {
    return !peer->touched || (peer->kind == kind && peer->bw == 0);
}

/* Whether the peer is known to run its own passband of demodulator @p kind as read: left on it
 * (rigctl_peer_left_on_own()) with a reading behind the 0. A 0 sent where no reading was left may have left the peer
 * on this client's passband, since SDR++ and GQRX keep the passband they had, so a reading is not retired on it: a new
 * read would take that passband for the peer's own (issue #621). */
static int
rigctl_runs_own_passband(const rigctl_peer* peer, int kind) {
    return rigctl_peer_left_on_own(peer, kind) && (!peer->touched || peer->demod[kind].own_hz > 0);
}

/* Retire the reading of one demodulator. A return's retire (@p hold) keeps it in retired_hz for a revert that undoes
 * that return (rigctl_reinstate_retired_readings()), until the next request this client sends (rigctl_request()), the
 * next revert (rigctl_take_retired_readings()) or a reset of the demodulator's record (rigctl_demod_reset()). */
static void
rigctl_retire_demod_reading(rigctl_demod* demod, int hold) {
    const int own_hz = demod->own_hz;
    rigctl_demod_reset(demod);
    demod->retired_hz = hold ? own_hz : 0;
}

/* Take what the last request's return held for a revert (@p retired, per demodulator), leaving nothing held: only the
 * revert of that return can bring it back, and that revert consumes it whichever way it goes (RevertModulation()). */
static void
rigctl_take_retired_readings(rigctl_peer* peer, int* retired) {
    for (int k = DSD_ANALOG_DEMOD_FM; k <= DSD_ANALOG_DEMOD_AM; k++) {
        retired[k] = peer->demod[k].retired_hz;
        peer->demod[k].retired_hz = 0;
    }
}

/* A revert put back a passband this client set, undoing the return that retired @p retired (as
 * rigctl_take_retired_readings() took it): each retired reading of a demodulator that has none comes back for the
 * session (RevertModulation()). */
static void
rigctl_reinstate_retired_readings(rigctl_peer* peer, const int* retired) {
    for (int k = DSD_ANALOG_DEMOD_FM; k <= DSD_ANALOG_DEMOD_AM; k++) {
        rigctl_demod* demod = &peer->demod[k];
        if (demod->reading == RIGCTL_READING_NONE && retired[k] > 0) {
            demod->reading = RIGCTL_READING_SESSION;
            demod->own_hz = retired[k];
        }
    }
}

/* The session's need of its reading of demodulator @p kind has ended: the peer is known to run its own passband of it
 * again, which the operator may change before this client next changes it. The reading is retired, never to be sent
 * again as a return, so the next change reads afresh (rigctl_read_own_passband()); the other demodulator's reading the
 * same lookup took in passing, which no change of this client's holds, goes with it (issue #621). A scan's readings
 * last the scan (issue #526), and a reading an undo of this client's still has to send stays. A return's retire
 * (@p hold) keeps each reading for a revert that undoes the return (rigctl_retire_demod_reading()). */
static void
rigctl_retire_session_reading(rigctl_peer* peer, int kind, int hold) {
    if (peer->demod[kind].reading != RIGCTL_READING_SESSION) {
        return;
    }
    rigctl_retire_demod_reading(&peer->demod[kind], hold);
    rigctl_demod* other = &peer->demod[rigctl_other_kind(kind)];
    if (other->reading == RIGCTL_READING_SESSION && !other->changed) {
        rigctl_retire_demod_reading(other, hold);
    }
}

/* The passband just asked of the peer for demodulator @p kind (SetScanRowModulation()) is the session's, not a scan
 * row's (issue #621): its reading serves the session, and so does the other demodulator's that the same lookup took in
 * passing and that nothing of this client's holds. A session request the peer refused while it runs its own passband
 * as read leaves that reading of no use to a return, and the operator may change the passband before the next
 * request, so it is retired at once; one the peer took, or whose reply was lost, keeps the reading for the return.
 * Nothing is held for a revert: the request was for a passband, not a return, and a revert undoes no return here. */
static void
rigctl_mark_session(rigctl_peer* peer, int kind) {
    if (peer->demod[kind].reading == RIGCTL_READING_SCAN) {
        peer->demod[kind].reading = RIGCTL_READING_SESSION;
    }
    rigctl_demod* other = &peer->demod[rigctl_other_kind(kind)];
    if (other->reading == RIGCTL_READING_SCAN && !other->changed) {
        other->reading = RIGCTL_READING_SESSION;
    }
    if (rigctl_runs_own_passband(peer, kind)) {
        rigctl_retire_session_reading(peer, kind, 0);
    }
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
    /* A request supersedes what the last return held for a revert, whatever the answer: the revert reads the holds
       first (RevertModulation()), and a return sets them anew below. */
    peer->demod[DSD_ANALOG_DEMOD_FM].retired_hz = 0;
    peer->demod[DSD_ANALOG_DEMOD_AM].retired_hz = 0;
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
    rigctl_demod* demod = &peer->demod[kind];
    if (rc == -1) {
        /* The request may have reached the peer before the reply was lost, so what it runs is no longer known: no
         * request matches the cache until one is answered, the FM undo at the peer's own passband included. A request
         * for the other demodulator leaves the peer on either one, so a best-effort tune that reads it
         * (CachedModulationKind()) fails until the peer accepts one. A passband asked for may stand. */
        peer->touched = 1;
        if (peer->kind != kind) {
            peer->kind = DSD_RIGCTL_KIND_UNKNOWN;
        }
        peer->bw = INT_MIN;
        if (cache_bw > 0) {
            demod->changed = 1;
        }
    } else if (rc == 1) {
        peer->touched = 1;
        peer->unconfirmed = 0;
        peer->kind = kind;
        peer->bw = cache_bw;
        if (cache_bw > 0) {
            demod->changed = 1;
        } else if (cache_bw == 0 && send_bw > 0 && send_bw == demod->own_hz) {
            /* The peer took its own passband back as read: nothing of this client's stands on the demodulator. A
             * reading the session's passband held is retired (issue #621); a scan's lasts the scan, which reads each
             * passband once (issue #526). A return sent as 0 (no reading) ends nothing: SDR++ and GQRX keep the
             * passband they had, so the peer may still run this client's, which a new read would take for its own. */
            demod->changed = 0;
            rigctl_retire_session_reading(peer, kind, 1);
        }
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

/* Look up the peer's own passband of demodulator @p kind before this client first changes it: once per reading, which
 * lasts the scan, or the session's need of it (rigctl_retire_session_reading()). The reading is a scan's until a
 * session request marks it (rigctl_mark_session()). A peer running the other demodulator is switched to this one at
 * passband 0, which keeps its own, and asked again; the passband of the one it ran is read in passing too, unless it
 * has a reading already. A peer that cannot say leaves it not known (0), and is not asked again for this reading. */
static void
rigctl_read_own_passband(rigctl_peer* peer, int kind, char* buf) {
    rigctl_demod* demod = &peer->demod[kind];
    if (demod->reading != RIGCTL_READING_NONE) {
        return;
    }
    demod->reading = RIGCTL_READING_SCAN;
    demod->own_hz = 0;
    int now_kind = -1;
    int passband = 0;
    if (rigctl_get_mode(peer->sockfd, buf, &now_kind, &passband) != 1) {
        return;
    }
    if (now_kind == kind) {
        demod->own_hz = passband;
        return;
    }
    if (now_kind >= 0 && peer->demod[now_kind].reading == RIGCTL_READING_NONE) {
        peer->demod[now_kind].reading = RIGCTL_READING_SCAN;
        peer->demod[now_kind].own_hz = passband;
    }
    if (rigctl_request(peer, kind, 0, 0, buf) == 1 && rigctl_get_mode(peer->sockfd, buf, &now_kind, &passband) == 1
        && now_kind == kind) {
        demod->own_hz = passband;
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
        /* FM at the peer's own passband only undoes what this client changed on the socket: a passband it set on FM,
         * or the switch to AM it made (an am row's demodulator). A peer left on FM at its own passband
         * (rigctl_peer_left_on_own(): never asked anything, or FM at 0 the last request it accepted, a reconnect since
         * or not) has nothing of this client's to undo: nothing is sent. A reading the session's passband held was
         * retired when the peer took it back, or refused the session's request while on its own
         * (rigctl_mark_session()); none is left to retire here. */
        if (rigctl_peer_left_on_own(peer, DSD_ANALOG_DEMOD_FM)) {
            return true;
        }
        /* The passband read before this client changed it; where the peer could not say, or the reading was retired,
         * Hamlib's 0 (normal passband). */
        send_bw = peer->demod[DSD_ANALOG_DEMOD_FM].own_hz;
    } else if (peer->kind == want_kind && peer->bw == bandwidth && !peer->unconfirmed) {
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
    /* What the tune's last request retired, if it was a return: this revert consumes it, whichever way it goes. */
    int retired[2];
    rigctl_take_retired_readings(peer, retired);
    if (peer->kind == before.kind && peer->bw == before.bandwidth) {
        return true; /* the tune changed nothing this client knew of */
    }
    if (before.kind != DSD_ANALOG_DEMOD_FM && before.kind != DSD_ANALOG_DEMOD_AM) {
        return false; /* no demodulator known to go back to */
    }
    /* The peer's own passband of that demodulator, read before a row changed it (0: not read). */
    const int own_bw = peer->demod[before.kind].own_hz;
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
    const int rc = rigctl_request(peer, before.kind, send_bw, cache_bw, buf);
    /* Putting back a passband this client set undoes the return the failed tune made before its frequency, where that
     * return was the tune's last request (otherwise nothing is held): the readings it retired come back for the
     * session, the other demodulator's included, since the operator had no time to change what the peer ran in
     * between. A revert whose reply was lost may have stood, so it reinstates too; one the peer refused leaves the
     * return in force (issue #621). */
    if (rc != 0 && before.bandwidth > 0) {
        rigctl_reinstate_retired_readings(peer, retired);
    }
    return rc == 1;
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
    if (rc != 0 && peer->demod[want_kind].reading == RIGCTL_READING_SESSION) {
        /* A scan row's change the peer answered (taken, or its reply lost) makes the reading the scan's; a session
         * request marks it again right after (RigctlMarkSessionPassband()). */
        peer->demod[want_kind].reading = RIGCTL_READING_SCAN;
    }
    return rc == 1;
}

void
RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind) {
    /* A socket the record does not describe was asked nothing this record knows of: there is nothing to mark. */
    if (s_peer.sockfd != sockfd || sockfd == DSD_INVALID_SOCKET) {
        return;
    }
    rigctl_mark_session(&s_peer, rigctl_kind(kind));
}

bool
RestoreScanModulation(dsd_socket_t sockfd, int kind, int bandwidth, bool session_passband) {
    rigctl_peer* peer = rigctl_peer_on(sockfd);
    const int want_kind = rigctl_kind(kind);
    char buf[BUFSIZE + 1];
    int restored[2] = {1, 1};
    /* The other demodulator's passband first, so that the request for what the session runs comes last and leaves the
     * peer on it; that request puts back its own passband where it asks for FM without -B. */
    for (int other = DSD_ANALOG_DEMOD_FM; other <= DSD_ANALOG_DEMOD_AM; other++) {
        const rigctl_demod* demod = &peer->demod[other];
        if (other != want_kind && demod->changed && demod->own_hz > 0) {
            restored[other] = rigctl_request(peer, other, demod->own_hz, 0, buf) == 1;
        }
    }
    /* A passband the session sets on the monitor the peer demodulates (its configured width, the AM default, or -B
     * standing in for an unset NFM width), or -B on a digital mode of audio input (issue #621), is one more passband
     * this client sets: it goes through the capturing call, which reads the peer's own passband first if no row did,
     * and what was read of that demodulator stays, so the session's later return to the peer's own sends it back rather
     * than 0, which SDR++ and GQRX take as "unchanged". */
    const bool ok = session_passband ? SetScanRowModulation(sockfd, want_kind, bandwidth)
                                     : SetModulationKind(sockfd, want_kind, bandwidth);
    restored[want_kind] = (ok && !session_passband) ? 1 : 0;
    /* The next scan reads the peer's own passbands again: the operator may change them between scans. A demodulator
     * whose undo the peer did not accept (refused, or its reply lost) may still run a row's passband, which a read
     * would take for the peer's own, so what was read of it stays for a later undo to send; so does the one the
     * session's own passband now runs on. */
    for (int k = DSD_ANALOG_DEMOD_FM; k <= DSD_ANALOG_DEMOD_AM; k++) {
        if (restored[k]) {
            rigctl_demod_reset(&peer->demod[k]);
        }
    }
    /* The reading of the session's demodulator now serves the session's passband, taken or not, as every session
       request is marked (RigctlMarkSessionPassband()): a return retires it once the peer accepts exactly that reading,
       and a request refused while the peer is known to run its own passband retires it at once. */
    if (session_passband) {
        rigctl_mark_session(peer, want_kind);
    }
    return ok;
}

/* A manual tune by rigctl: the demodulator and passband the session runs (@p state: the scan scope and the channel
 * map, if any, NULL when the caller has none), then the frequency. On audio input the peer demodulates (issue #621)
 * that is the passband the analog monitor runs -- its configured width, the AM default, or -B standing in for an unset
 * NFM width -- or the -B a digital mode is heard through, sent through the capturing call so a later return to the
 * peer's own sends the passband read; a bare -B would overwrite the configured passband, or the peer's own with nothing
 * read to go back to. A refusal fails the tune before the frequency moves, as a refused -B always has here. Anywhere
 * else -B is sent as it always was. Where the session runs the peer's own passband (-B 0, or the monitor with neither a
 * width nor -B) FM at 0 is asked for, best-effort: it undoes a passband this client set (a session width before a
 * switch to a digital mode, or one a refused live return left in force) with the passband read before it, and is a
 * no-op for a peer back on its own or never asked. */
static int
set_rigctl_frequency(const dsd_opts* opts, const dsd_state* state, long int freq) {
    if (opts->rigctl_sockfd == DSD_INVALID_SOCKET) {
        return -1;
    }
    const dsd_rigctl_passband passband = dsd_rigctl_passband_of(opts, NULL);
    if (dsd_rigctl_passband_captures(opts, &passband)) {
        const bool ok = SetScanRowModulation(opts->rigctl_sockfd, passband.kind, passband.bandwidth_hz);
        /* Off a scan the reading taken for it is the session's (issue #621), as every rigctl leg tells it: a return to
           the peer's own retires it. */
        if (dsd_channel_modes_rigctl_request_is_session(opts, state)) {
            RigctlMarkSessionPassband(opts->rigctl_sockfd, passband.kind);
        }
        if (!ok) {
            return -1;
        }
    } else if (opts->setmod_bw != 0) {
        if (!SetModulation(opts->rigctl_sockfd, opts->setmod_bw)) {
            return -1;
        }
    } else {
        (void)SetModulationKind(opts->rigctl_sockfd, DSD_ANALOG_DEMOD_FM, 0);
    }
    return SetFreq(opts->rigctl_sockfd, freq) ? 0 : -1;
}

#ifdef USE_RADIO
static int
set_rtl_frequency(dsd_opts* opts, const dsd_state* state, uint32_t requested_freq, uint32_t* applied_freq) {
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
 * @param state Decoder state (required for RTL tuning, may be NULL for rigctl-only, where it tells a scan's passband
 *              request from the session's, dsd_channel_modes_rigctl_request_is_session()).
 * @param freq Target frequency in Hz.
 * @return 0 on success, 1 when an RTL tune is deferred, or a negative error/timeout code. An RTL timeout leaves an
 *         accepted request active and retains its target in opts->rtlsdr_center_freq.
 */
int
io_control_set_freq(dsd_opts* opts, const dsd_state* state, long int freq) {
    if (!opts || freq <= 0) {
        return -1;
    }
    /* The tuner path carries a uint32_t: a frequency past it (possible where long is 64-bit) would wrap to an unrelated
       channel, so it is refused instead. */
    if ((long long)freq > (long long)UINT32_MAX) {
        LOG_ERROR("io_control: %ld Hz is past the tuner limit of 4294967295 Hz; not tuning\n", freq);
        return -1;
    }
    uint32_t applied_freq = (uint32_t)freq;

    LOG_INFO("io_control: tune to %ld Hz\n", freq);

    int rc = -1;
    if (opts->use_rigctl == 1) {
        rc = set_rigctl_frequency(opts, state, freq);
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
