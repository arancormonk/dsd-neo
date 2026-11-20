// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Private engine helpers the -Y scanner and the trunk-scan coordinator share for TETRA DMO rows (issue #890).
 *
 * One eligibility rule for a tetra-dmo row (-Y class tetra-dmo, trunk-scan type tetra-dmo-conventional), read where a
 * row is chosen or tuned, and the TETRA state's lifetime across a scan session's landings. Defined in scan_tetra.c.
 */
#ifndef DSD_NEO_SRC_ENGINE_SCAN_TETRA_INTERNAL_H_
#define DSD_NEO_SRC_ENGINE_SCAN_TETRA_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/runtime/tetra_rate.h>
#include <stddef.h>

/** Whether a tetra-dmo row can be visited, and if not why (dsd_engine_scan_tetra_row_check()). */
typedef enum {
    DSD_ENGINE_TETRA_ROW_OK = 0,
    DSD_ENGINE_TETRA_ROW_INPUT,        /**< no radio I/Q input DSD-neo tunes: never, this session */
    DSD_ENGINE_TETRA_ROW_RATE_UNKNOWN, /**< the radio publishes no DSP rate yet: it is opening or reopening */
    DSD_ENGINE_TETRA_ROW_RATE,         /**< the DSP rate is not one TETRA runs at */
    DSD_ENGINE_TETRA_ROW_LPF,          /**< DSD_NEO_CHANNEL_LPF=0 turns off the channel filter the chain runs */
    DSD_ENGINE_TETRA_ROW_TED,          /**< the TED timing the stream applied disagrees with the rate */
    DSD_ENGINE_TETRA_ROW_UPLINK,       /**< --tetra-uplink: the downlink chain never runs */
} dsd_engine_tetra_row;

/** Room for every dsd_engine_scan_tetra_row_text() text. */
#define DSD_ENGINE_TETRA_ROW_TEXT_MAX (DSD_TETRA_RATE_ADVICE_MAX + 32)

/**
 * @brief Whether a tetra-dmo row can be visited at DSP rate @p rate_hz (dsd_engine_scan_dsp_rate_hz(), 0 unknown), as
 * the TETRA downlink chain decides it runs (dsd_demod_tetra_chain_wanted()): a radio I/Q input DSD-neo tunes (an
 * RTL-SDR, rtl_tcp, SoapySDR or Airspy source; not audio input, a rigctl peer behind audio input, an I/Q replay or an
 * I/Q stream another program sends), no --tetra-uplink, the channel filter not forced off, a known DSP rate in TETRA's
 * domain (dsd_tetra_dsp_rate_runs()), and, once the row has @p landed, the TED timing the stream applied for it
 * (dsd_engine_scan_tetra_ted_sps(): the override in force and the TED's own samples per symbol) equal to the rate over
 * the symbol rate. Prospectively (@p landed 0: while another family is on air) the TED is not judged, since the row's
 * landing sets its own TED and clears the override another family's tune left. The same for every tetra-dmo row of the
 * session: nothing of a row's own takes part. A few loads; decoder thread.
 */
dsd_engine_tetra_row dsd_engine_scan_tetra_row_check(const dsd_opts* opts, const dsd_state* state, int rate_hz,
                                                     int landed);

/** The typed-frequency reason (dsd_scan_entry_reason) for @p why: DSD_SCAN_ENTRY_OK for DSD_ENGINE_TETRA_ROW_OK. */
dsd_scan_entry_reason dsd_engine_scan_tetra_entry_reason(dsd_engine_tetra_row why);

/** Whether @p why is a reason that recovers on its own (the rate the radio has not published yet), which a target on
 * air is not sent away for. */
static inline int
dsd_engine_scan_tetra_row_waits(dsd_engine_tetra_row why) {
    return why == DSD_ENGINE_TETRA_ROW_RATE_UNKNOWN;
}

/**
 * @brief What keeps a tetra-dmo row from being visited, @p why at DSP rate @p rate_hz, with the fix where there is one,
 * in @p out: the full text for the log (@p brief 0, "TETRA DMO needs a 72 kHz DSP bandwidth, not 48 kHz; ..."), or the
 * short one for the status line (@p brief 1). Empty for DSD_ENGINE_TETRA_ROW_OK.
 */
void dsd_engine_scan_tetra_row_text(const dsd_opts* opts, dsd_engine_tetra_row why, int rate_hz, int brief, char* out,
                                    size_t out_size);

/**
 * @brief A scan session lands on one of its rows (a trunk-scan park, a -Y row's commit): the TETRA state's part.
 *
 * A session that has a tetra-dmo row (@p session_has_tetra) takes the hold of the TETRA state extension
 * (dsd_state::tetra_ext_scan_held) at its first landing, so the presets of its other rows leave TETRA without freeing
 * it: a configured TETRA session's state is never freed by the scan either. While the session holds it, every landing
 * starts it cold (tetra_scan_visit_reset()): a TETRA row after another TETRA row, and a row of another family, whose
 * decoder leaves the state dormant and empty. Call after the outgoing carrier was left and before the incoming row's
 * decoder is entered. A session with no tetra-dmo row (an untyped -Y list under -fT among them) changes nothing.
 */
void dsd_engine_scan_tetra_landing(dsd_opts* opts, dsd_state* state, int session_has_tetra);

/**
 * @brief The scan session leaves its rows (a pause into manual decoding, a stop, a failed start or resume): let the
 * hold go. Call once the configured settings are back (the scope leave). The state the last row left goes
 * (tetra_scan_visit_reset()), so the session in force starts cold, and when its settings run no TETRA decoder the
 * extension is freed (dsd_decode_mode_leave_tetra()); a configured TETRA session keeps it. Nothing without the hold.
 */
void dsd_engine_scan_tetra_release(dsd_opts* opts, dsd_state* state);

#endif /* DSD_NEO_SRC_ENGINE_SCAN_TETRA_INTERNAL_H_ */
