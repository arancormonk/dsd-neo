// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The demodulator and passband a rigctl peer that demodulates audio input is asked for (issues #526, #621).
 *
 * On PCM audio input (Pulse, stdin, WAV, UDP, TCP) with --rigctl the peer (SDR++, GQRX, Hamlib) demodulates what
 * DSD-neo hears, so the analog channel width is the passband the peer is asked for. One rule, keyed on the analog
 * demodulator in force, decides it whenever the peer demodulates the analog monitor: the row's own width of that kind,
 * else the configured width of that kind; then for AM its 6 kHz default, and for FM -B (opts->setmod_bw), else the
 * peer's own passband. Everything else -- a digital mode, an RTL-family input whose I/Q DSD-neo demodulates itself, a
 * symbol-file or null input -- follows with FM at -B, best-effort, as every tune did before the peer demodulated. On
 * PCM input a digital mode is still heard through the peer's FM passband, so -B there is captured like a passband this
 * client sets (dsd_rigctl_passband_captures()): the peer's own passband is read before it is overwritten.
 *
 * Pure: it reads the options in force (the tuned row's, while a scan has one installed) and the row's options, and
 * asks nothing of the peer. Whether a rigctl peer is connected at all is the caller's to check
 * (dsd_opts_rigctl_live(), dsd_opts_rigctl_peer_demodulates()).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RIGCTL_PASSBAND_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RIGCTL_PASSBAND_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/runtime/scan_options.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Where the passband a rigctl peer is asked for comes from. */
typedef enum {
    /** No analog monitor the peer demodulates (digital mode; radio, symbol or null input): FM at -B, best-effort. */
    DSD_RIGCTL_PASSBAND_FOLLOW = 0,
    /** The row's own --nfm-bandwidth-hz or --am-bandwidth-hz. */
    DSD_RIGCTL_PASSBAND_ROW,
    /** The configured width of the kind in force (--nfm-bandwidth-hz, --am-bandwidth-hz, [analog]). */
    DSD_RIGCTL_PASSBAND_CONFIGURED,
    /** The AM monitor with no AM width set: the AM default (6 kHz). */
    DSD_RIGCTL_PASSBAND_AM_DEFAULT,
    /** The FM monitor with no NFM width set: -B stands in. */
    DSD_RIGCTL_PASSBAND_SETMOD_BW,
    /** The FM monitor with neither an NFM width nor -B: passband 0, the peer's own. */
    DSD_RIGCTL_PASSBAND_PEER_OWN
} dsd_rigctl_passband_source;

/** @brief One request to a rigctl peer: a demodulator, a passband and where the passband came from. */
typedef struct {
    int kind;         /**< dsd_analog_demod: DSD_ANALOG_DEMOD_FM or DSD_ANALOG_DEMOD_AM. */
    int bandwidth_hz; /**< The passband in Hz; 0: the peer's own. */
    int source;       /**< dsd_rigctl_passband_source. */
} dsd_rigctl_passband;

/**
 * @brief The request for the analog monitor of demodulator @p kind that the peer demodulates.
 *
 * @p row_hz > 0 gives ROW; else @p configured_hz > 0 gives CONFIGURED; else AM gives AM_DEFAULT at the AM default
 * width; else @p setmod_bw_hz > 0 gives SETMOD_BW; else PEER_OWN at 0. A @p kind other than AM is FM.
 *
 * @param kind          The demodulator in force (dsd_analog_demod).
 * @param row_hz        The row's own width of that kind in Hz, 0 for none.
 * @param configured_hz The configured width of that kind in Hz, 0 when unset.
 * @param setmod_bw_hz  -B in Hz, 0 when off.
 */
dsd_rigctl_passband dsd_rigctl_passband_for_kind(int kind, int row_hz, int configured_hz, int setmod_bw_hz);

/**
 * @brief The request the options in force @p opts and the row's options @p row (NULL = none) make of a rigctl peer.
 *
 * FOLLOW, FM at opts->setmod_bw, on any input but PCM audio (dsd_opts_input_is_pcm_audio()) or off the analog family
 * (dsd_opts_is_analog_family()). Otherwise dsd_rigctl_passband_for_kind() for opts->analog_demod, with @p row's own
 * width when it sets one of that kind (DSD_SCAN_OPT_BANDWIDTH and channel_bw_kind) and the configured width of that
 * kind in force (opts->analog_nfm_bandwidth_hz or opts->analog_am_bandwidth_hz). While a row with a width of its own is
 * installed the in-force width is that row's too, so the row is asked first: its request is ROW. Rigctl liveness is
 * not checked. NULL @p opts gives FOLLOW at 0.
 */
dsd_rigctl_passband dsd_rigctl_passband_of(const dsd_opts* opts, const dsd_scan_option_values* row);

/**
 * @brief 1 when @p p is a width (ROW, CONFIGURED or AM_DEFAULT): a strict request, whose refusal fails a tune because
 * the row would be heard through the wrong passband or demodulator. 0 otherwise and for NULL.
 */
int dsd_rigctl_passband_is_width(const dsd_rigctl_passband* p);

/**
 * @brief 1 when @p p is a passband this client sets on a monitor the peer demodulates: a width, or -B standing in for
 * an unset NFM width (SETMOD_BW). It goes through SetScanRowModulation(), which reads the peer's own passband ("m")
 * before the demodulator's first change, so a later return to the peer's own sends the passband read instead of
 * "M NFM 0", which SDR++ and GQRX take as "unchanged". 0 otherwise (FOLLOW, PEER_OWN) and for NULL.
 */
int dsd_rigctl_passband_sets_passband(const dsd_rigctl_passband* p);

/**
 * @brief 1 when the request @p p under the options @p opts overwrites a passband of the peer that demodulates the
 * input, so the peer's own passband is read first and the record of it kept for the return (SetScanRowModulation();
 * RestoreScanModulation() with session_passband): a passband this client sets on the monitor
 * (dsd_rigctl_passband_sets_passband()), or FOLLOW at -B (> 0) on PCM audio input (dsd_opts_input_is_pcm_audio()),
 * where the peer's FM passband is what a digital mode is heard through. 0 for FOLLOW on any other input (the peer only
 * follows the frequency), for FOLLOW at 0 and PEER_OWN (the peer's own passband, which is sent only to undo), and for
 * NULL. Whether a request is strict or best-effort is the caller's, as before: this says only whether it is captured.
 */
int dsd_rigctl_passband_captures(const dsd_opts* opts, const dsd_rigctl_passband* p);

/**
 * @brief 1 when @p a and @p b are the same request from the same source: the same demodulator, passband and source.
 * The source counts, so entering the monitor from FOLLOW is a change even where the wire request matches. Two NULLs are
 * equal; NULL and a request are not.
 */
int dsd_rigctl_passband_equal(const dsd_rigctl_passband* a, const dsd_rigctl_passband* b);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RIGCTL_PASSBAND_H_ */
