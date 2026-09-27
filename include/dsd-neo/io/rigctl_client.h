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
 * starts the cache of a new connection on a closed socket's number empty.
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

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H */
