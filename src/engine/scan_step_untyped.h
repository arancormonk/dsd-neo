// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Engine-private pieces of the untyped -Y step (a list without modes; issue #671), which the timed step and a resume
 * share: finding the next row, entering it, and settling a row whose RTL tune timed out. A row commits (the cursor past
 * it, its keys, the analog reset, fresh scan clocks) only once its tune has landed, after the carrier boundary ends the
 * calls of the row left.
 */

#ifndef DSD_NEO_SRC_ENGINE_SCAN_STEP_UNTYPED_H
#define DSD_NEO_SRC_ENGINE_SCAN_STEP_UNTYPED_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

/* dsd_engine_scan_untyped_enter_row() results. */
enum {
    DSD_UNTYPED_STEP_OK = 0,           /* Landed: the outgoing boundary ran and the row is committed. */
    DSD_UNTYPED_STEP_PENDING = 1,      /* The RTL tune timed out: noted, the row commits if it lands while the scan is
                                          still the same (dsd_engine_scan_untyped_poll()). A rigctl leg that moved first
                                          already ran the outgoing boundary. */
    DSD_UNTYPED_STEP_FAILED_MOVED = 2, /* A leg moved the receiver and a later one failed: the outgoing boundary ran and
                                          the tone heard went, but the row is not committed. */
    DSD_UNTYPED_STEP_FAILED = 3,       /* Nothing moved. */
};

/* The first row at or after @p from_roll (wrapping; one outside the list starts at row 0) that is not avoided, and with
   @p skip_placeholders, that carries a frequency (a 0 Hz row parks the timed step where it is, but a resume has no
   "here" to park on). -1 when there is none. */
int dsd_engine_scan_untyped_find_row(const dsd_state* state, int from_roll, int skip_placeholders);

/* Tune every configured front end to @p row and commit it as the timed step does (see the results above), with @p now
   the decode clock's wall second (dsd_decode_time()) the row's clocks start from. A failure, pending or not, reopens
   the scan's windows as an abandoned step does. A placeholder row only parks (committed, no tune, no boundary). Runs
   the SCAN_STEP carrier boundary, which takes no tick guard. Decoder thread. */
int dsd_engine_scan_untyped_enter_row(dsd_opts* opts, dsd_state* state, int row, double now);

/* The engine loop's poll of a manual tune that timed out (dsd_engine_manual_tune_poll()). An untyped step's row commits
   when its tune lands while its scan is still current: a -Y scan without modes in the same session
   (dsd_state::lcn_scan_session), on the same map (trunk_chan_map_seq), the row still on the frequency tuned. A failure
   reopens the scan's windows, as a step that failed in its wait does. A row whose scan is no longer current commits
   nothing: the poll's reconcile of the cached frequency is all. Returns the poll's result. Decoder thread. */
int dsd_engine_scan_untyped_poll(dsd_opts* opts, dsd_state* state);

#endif /* DSD_NEO_SRC_ENGINE_SCAN_STEP_UNTYPED_H */
