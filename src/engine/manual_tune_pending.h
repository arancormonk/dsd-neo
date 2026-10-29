// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Engine-private intake of manual-tune completions (issue #671): the RTL completion callback hands every request ID
 * with RTL_STREAM_TUNE_ID_MANUAL_BIT here, and every other ID to the trunk-tuning ledger. The record itself is
 * <dsd-neo/engine/manual_tune.h>.
 */

#ifndef DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H
#define DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H

#include <dsd-neo/engine/manual_tune.h>
#include <stdint.h>

/* What a noted tune was, as the poll that settled it took it. */
typedef struct {
    dsd_manual_tune_kind kind; /* 0 before any tune settled */
    uint32_t requested_hz;
    int row;
    uint32_t scan_session;
    uint64_t map_seq;
} dsd_manual_tune_settled;

/* The record the last dsd_engine_manual_tune_poll() that returned DSD_MANUAL_TUNE_LANDED or DSD_MANUAL_TUNE_FAILED took,
   for the caller to settle what the tune was for (an untyped -Y step's row). Decoder thread. */
void dsd_engine_manual_tune_last_settled(dsd_manual_tune_settled* out);

/* Store the completion of the manual tune @p id (an rtl_stream_tune_result) for the decoder's next poll. Called on
   the RTL controller thread, or on the thread stopping the stream; never blocks. */
void dsd_engine_manual_tune_completion(uint64_t id, int result);

#endif /* DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H */
