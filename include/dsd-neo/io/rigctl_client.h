// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H

#include <dsd-neo/platform/sockets.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a TCP connection to @p hostname:@p portno with the rigctl receive timeout. The rigctl peer's record
 * (SetModulationKind()) starts empty for a connection on the number of a socket closed before it; a connection on
 * another number, such as the TCP audio input's own (re)connect while the rigctl socket stays open, leaves the record
 * of that open socket alone.
 */
dsd_socket_t Connect(char* hostname, int portno);

/** The longest ConnectBounded() waits for a connection: the bound rtl_tcp connects within too. */
#define DSD_CONNECT_BOUNDED_TIMEOUT_MS 10000U

/**
 * @brief Connect() within DSD_CONNECT_BOUNDED_TIMEOUT_MS, asking @p cancelled every 100 ms
 * (dsd_socket_connect_bounded()), with the same receive timeout and options (issue #634): the TCP audio input's menu
 * connect and its reconnect, which must not hold the decoder thread for the system's connect timeout.
 *
 * @p resolve 1 looks @p hostname up; 0 connects to the address the running TCP audio input's connection went to when
 * it is for this host and port (ConnectKeepTcpAudioAddress()), looking the host up only when it is not. A reconnect
 * passes 0, so a host whose resolver stalls cannot hold the decoder thread on each attempt. Decoder thread only.
 * Returns the connected socket, or DSD_INVALID_SOCKET when the lookup or the connection failed, timed out or was
 * cancelled.
 */
dsd_socket_t ConnectBounded(const char* hostname, int portno, int resolve, dsd_socket_cancel_fn cancelled,
                            void* context);

/**
 * @brief The TCP audio input now runs on the connection Connect() or ConnectBounded() last made, to @p hostname:@p portno
 * (issue #634): its reconnect goes back to that address (ConnectBounded() with resolve 0). Call it once the input that
 * connection serves is installed, at startup and after a switch that took it; a connection that is not kept (a switch
 * that failed, a rigctl connection) leaves the address the running input's reconnect uses alone. Decoder thread only.
 */
void ConnectKeepTcpAudioAddress(const char* hostname, int portno);
long int GetCurrentFreq(dsd_socket_t sockfd);
bool SetFreq(dsd_socket_t sockfd, long int freq);
bool SetModulation(dsd_socket_t sockfd, int bandwidth);
/**
 * @brief Ask the rigctl peer to demodulate @p kind (dsd_analog_demod: DSD_ANALOG_DEMOD_FM or DSD_ANALOG_DEMOD_AM) at
 * passband @p bandwidth Hz (issue #526).
 *
 * FM sends "M NFM <bandwidth>", then "M FM <bandwidth>" if the peer refuses that token (SetModulation() is this call
 * for FM); AM sends "M AM <bandwidth>". Each socket caches the demodulator and passband its peer last accepted, keyed
 * on both, and a request for what the peer runs skips the I/O. An FM @p bandwidth of 0 asks for the peer's own
 * passband: it is sent only to undo a request this client made on the socket (an AM scan row's demodulator, or a row's
 * own passband), and skipped on a socket nothing was asked of, whose peer keeps its own settings. It sends the passband
 * SetScanRowModulation() read from the peer before a row changed it, since SDR++ and GQRX take a passband of 0 as
 * "unchanged"; only where the peer could not say is it sent as 0 (Hamlib's normal passband). Returns false when the
 * peer refuses the request or the I/O fails. After a refusal the cache keeps what the peer last accepted, so the same
 * request asks again. After an I/O failure the request may have reached the peer, so what it runs is not known: the
 * next request on the socket is sent whatever it asks for, the FM undo included, and a request for the other
 * demodulator than the one the peer last accepted leaves that not known either (DSD_RIGCTL_KIND_UNKNOWN). Connect()
 * starts the cache of a new connection on a closed socket's number empty, and RigctlRebindPeer() hands a reconnect
 * what the socket it replaces knew.
 */
bool SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);

/** CachedModulationKind() of a peer that may run either demodulator: a request for the other one lost its reply. */
#define DSD_RIGCTL_KIND_UNKNOWN (-1)

/**
 * @brief The demodulator (dsd_analog_demod) the rigctl peer on @p sockfd runs as far as this client knows (issue #526),
 * without I/O: the last one the peer accepted from SetModulationKind() or a scan call, which a refused request leaves
 * in force, as does a lost reply to a request for that same demodulator. DSD_RIGCTL_KIND_UNKNOWN after a lost reply to
 * a request for the other one, until the peer accepts a request again. DSD_ANALOG_DEMOD_FM on a socket nothing was
 * asked of.
 *
 * A scan tune on audio input that asks for FM best-effort (-B, or the peer's own passband) reads it after a refusal: a
 * peer still on the AM an am row put it on, or one that may be, would give the row being tuned the wrong demodulator,
 * so that tune fails instead.
 */
int CachedModulationKind(dsd_socket_t sockfd);

/** What the rigctl peer on a socket runs as far as this client knows (issue #526), for RevertModulation(). */
typedef struct {
    int kind;      /**< dsd_analog_demod, or DSD_RIGCTL_KIND_UNKNOWN. */
    int bandwidth; /**< The passband in Hz; 0: the peer's own; INT_MIN: not known. */
} dsd_rigctl_modulation;

/** @brief What the rigctl peer on @p sockfd runs as far as this client knows, without I/O (issue #526): taken before a
 * tune's modulation request, for RevertModulation() to put back should the tune fail. */
dsd_rigctl_modulation CachedModulation(dsd_socket_t sockfd);

/**
 * @brief Put back @p before (a CachedModulation() of @p sockfd) once a tune failed after its modulation request
 * changed what the peer runs (issue #526): the row still on air would otherwise be received through the demodulator or
 * passband of the row that could not be tuned.
 *
 * Nothing is sent when the peer runs @p before. Where @p before knew no passband (a peer nothing was asked of, or a
 * lost reply) and the peer still runs its demodulator, the passband to go back to is the peer's own, read before a row
 * changed it (the first nfm row's own passband, say): nothing is sent when none was read or the peer runs its own
 * passband again, and the cache holds it as the peer's own (0) once accepted. Otherwise the peer is asked for
 * @p before's demodulator at @p before's passband, its own passband (as SetModulationKind() sends it) for 0 or one not
 * known. Returns true when nothing was to be sent or the peer accepted the request; false when it refused, the I/O
 * failed, or @p before knew no demodulator to go back to.
 */
bool RevertModulation(dsd_socket_t sockfd, dsd_rigctl_modulation before);
/**
 * @brief A scan row's own request (issue #526): an am row's AM at its width, or an nfm row's own passband, @p bandwidth
 * Hz (> 0), which the scan puts back once it leaves (RestoreScanModulation()).
 *
 * Before the first such request of a demodulator on the socket since the scan's last restore, the peer is asked which
 * demodulator and passband it runs ("m"); a peer on the other demodulator is first switched to this one at passband 0,
 * which keeps its own, so that passband can be read too. That read is the peer's own passband, which the FM undo and
 * the restore send back explicitly. A peer that cannot answer "m" leaves it not known, and the undo falls back to 0.
 * Then the request is sent as SetModulationKind() sends it. Returns false when the peer refuses it or the I/O fails.
 */
bool SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth);
/**
 * @brief Once a scan has left its rows, put back each passband a scan row changed on the socket (issue #526), then ask
 * for what the session runs, @p kind at @p bandwidth, as SetModulationKind() would.
 *
 * The passband of the demodulator the session does not run goes back first (a peer that keeps a passband per
 * demodulator, as SDR++ does, would otherwise keep an am row's width for AM), so the session's request comes last and
 * leaves the peer on its demodulator; FM without -B then puts back the peer's own FM passband. A passband whose read
 * failed cannot be put back. The next scan reads the peer's own passbands again, except those of a demodulator whose
 * undo the peer did not accept (refused, or its reply lost): that one may still run a row's passband, so what was read
 * of it stays for the next undo or restore to send. Returns what the session's request returns.
 */
bool RestoreScanModulation(dsd_socket_t sockfd, int kind, int bandwidth);
/**
 * @brief Hand a rigctl reconnect what the socket it replaces knew of its peer (issue #589), without I/O.
 *
 * Call it once Connect() returned @p new_fd while @p old_fd is still open, so the two numbers differ, then close
 * @p old_fd before anything is sent on @p new_fd (a peer may serve one client at a time). Nothing may send on either
 * socket meanwhile, or a reply to the old one would land in the new one's record: the reconnect service holds the P25
 * SM tick guard, under which the watchdog's retunes run. Without it the new socket is a peer nothing was asked of: a
 * scan whose am row left the peer on AM would send no FM undo and no restore. The frequency SetFreq() last sent is
 * forgotten too, since a later connection can get the closed socket's number back.
 *
 * - No record of @p old_fd, or none of a request that may have changed its peer (nothing it accepted, or
 *   @p old_fd is DSD_INVALID_SOCKET): the new socket is a peer nothing was asked of, as after a first Connect(), which
 *   keeps its own settings (FM at its own passband is not sent to it).
 * - @p same_endpoint (the same host and port): the new socket takes the record, so the FM undo and
 *   RestoreScanModulation() still send the peer's own passbands read before a scan row changed them, and a tune that
 *   fails still puts back what the peer last accepted (CachedModulation()). The peer may have restarted or been changed
 *   while the connection was down, so no request is taken for one it already runs until it accepts one on the new
 *   socket.
 * - Another endpoint, with the old peer possibly on AM (an am row's, or either demodulator after a lost reply): it
 *   may be the same peer under another name, so the new socket's demodulator is not known (DSD_RIGCTL_KIND_UNKNOWN,
 *   passband not known). The next FM request is sent, the peer's own passband (0) included, a best-effort tune the
 *   peer refuses fails (CachedModulationKind()), and the restore asks for FM. None of the old peer's own passbands
 *   are sent to it.
 * - Another endpoint, with the old peer on FM: a fresh record, as after a first Connect(), so a peer that refuses mode
 *   requests does not fail every best-effort tune. The same peer reached under another name keeps a row passband it
 *   still runs.
 *
 * Whatever it hands over, the record names @p new_fd afterwards, never the old socket closed next, whose number a
 * later connection (the TCP audio input's) may get back. The engine calls it with DSD_INVALID_SOCKET for @p old_fd
 * once it opens a run's first connection, so the record names that connection before the P25 watchdog starts.
 */
void RigctlRebindPeer(dsd_socket_t old_fd, dsd_socket_t new_fd, int same_endpoint);
#ifdef DSD_NEO_TEST_HOOKS
/* The socket the rigctl peer record describes (DSD_INVALID_SOCKET: none). */
dsd_socket_t dsd_rigctl_test_record_socket(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H */
